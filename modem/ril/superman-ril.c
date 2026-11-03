// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../qmux-codec.h"
#include "calls.h"
#include "model.h"
#include "qmux.h"
#include "sim-io.h"
#include "subscription.h"
#include "sms.h"
#include <android/log.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <telephony/ril.h>
#include <time.h>
#include <unistd.h>
/* Android logical slot1 uses the physically accessible SIM2 selected for bring-up. */
#define PHYSICAL_SLOT 1
#define SUBSCRIPTION 0
#define GW_SESSION (SUBSCRIPTION * 2)
static const struct RIL_Env *environment;
static struct qmux transport = {.fd = -1, .control = -1};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int radio_state = RADIO_STATE_UNAVAILABLE;
static pthread_t thread;
static uint8_t previous_card[4096];
static size_t previous_card_bytes;
static struct serving_status previous_serving;
static int previous_serving_valid;
struct call_snapshot {
    int id, state, incoming, multiparty, presentation;
    char number[82];
};
static struct call_snapshot previous_calls[SUPERMAN_MAX_CALLS];
static unsigned previous_call_count;
static int previous_calls_valid;
static int calls_get(struct call_list *calls);
static void radio_indication(uint8_t service,uint16_t message,const uint8_t *body,size_t bytes);
static int stopping, started, activation_attempted;
static struct sms_queue incoming_sms;
static int messaging_attempted, messaging_ready, messaging_primary;
static volatile sig_atomic_t terminate_requested;
static void terminate_handler(int signal_number) {
    (void)signal_number;
    terminate_requested = 1;
}
static int request(unsigned service, unsigned message, const void *data, size_t bytes,
                   const uint8_t **body, size_t *body_bytes) {
    int client = qmux_client(&transport, service);
    if (client < 0)
        return client;
    int rc = qmux_request(&transport, service, client, message, data, bytes, body, body_bytes);
    if (rc < 0)
        __android_log_print(ANDROID_LOG_WARN, "SupermanRIL", "QMI service=%u message=%04x error=%d",
                            service, message, rc);
    return rc;
}
static RIL_Errno modem_error(int rc) {
    if (rc == -EBADMSG)
        return RIL_E_INVALID_RESPONSE;
    if (rc < 0)
        return RIL_E_RADIO_NOT_AVAILABLE;
    return rc ? RIL_E_MODEM_ERR : RIL_E_SUCCESS;
}
static int mode_get(void) {
    const uint8_t *body;
    size_t bytes, len;
    int rc = request(2, 0x2d, NULL, 0, &body, &bytes);
    if (rc) {
        __android_log_print(ANDROID_LOG_WARN, "SupermanRIL", "DMS mode query error=%d", rc);
        return -1;
    }
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    if (!v || len != 1)
        __android_log_print(ANDROID_LOG_WARN, "SupermanRIL",
                            "DMS mode response missing/invalid TLV");
    return v && len == 1 ? v[0] : -1;
}
static int64_t monotonic_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int mode_set(unsigned mode) {
    uint8_t args[] = {1, 1, 0, mode};
    const uint8_t *body;
    size_t bytes;
    int rc = request(2, 0x2e, args, sizeof(args), &body, &bytes);
    if (rc)
        return rc;
    int64_t start = monotonic_ms();
    if (start < 0)
        return -EIO;
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < start)
            return -EIO;
        if (now - start >= 5000)
            return -ETIMEDOUT;
        int actual = mode_get();
        if (actual == (int)mode)
            return 0;
        if (actual < 0)
            return -EIO;
        usleep(100000);
    }
}
static int subscriptions_get(struct subscription_state *state) {
    const uint8_t *body;
    size_t bytes;
    int rc = request(3, 0x5c, NULL, 0, &body, &bytes);
    return rc ? rc : subscription_decode(body, bytes, state);
}
static int subscriptions_set(const struct subscription_state *state) {
    uint8_t args[12];
    int n = subscription_request(args, sizeof(args), state);
    if (n < 0)
        return n;
    const uint8_t *body;
    size_t bytes;
    int rc = request(3, 0x4b, args, n, &body, &bytes);
    if (rc)
        return rc;
    int64_t start = monotonic_ms();
    if (start < 0)
        return -EIO;
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < start)
            return -EIO;
        if (now - start >= 5000)
            return -ETIMEDOUT;
        struct subscription_state actual;
        rc = subscriptions_get(&actual);
        if (rc)
            return rc;
        if (actual.standby == state->standby && actual.priority == state->priority &&
            actual.data == state->data && actual.voice == state->voice)
            return 0;
        usleep(100000);
    }
}
static int select_subscription(void) {
    struct subscription_state original;
    int rc = subscriptions_get(&original);
    if (rc)
        return rc;
    if (original.priority == SUBSCRIPTION && original.data == SUBSCRIPTION &&
        original.voice == SUBSCRIPTION) {
        __android_log_print(ANDROID_LOG_INFO, "SupermanRIL",
                            "SIM2 primary logical subscription already verified");
        return 0;
    }
    int mode = mode_get();
    /* Fresh MPSS reports SHUTDOWN; initialize low power, never request shutdown. */
    if (mode == 5)
        mode = 1;
    if (mode != 0 && mode != 1)
        return -EIO;
    struct subscription_state target = original;
    target.priority = target.data = target.voice = SUBSCRIPTION;
    rc = mode_set(1);
    if (!rc)
        rc = subscriptions_set(&target);
    if (!rc)
        rc = mode_set(mode);
    if (rc) {
        int off = mode_set(1);
        int restore = subscriptions_set(&original);
        int power = mode_set(mode);
        __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL",
                            "Subscription assignment error=%d rollback RFoff=%d prefs=%d RFmode=%d",
                            rc, off, restore, power);
    } else {
        __android_log_print(
            ANDROID_LOG_INFO, "SupermanRIL",
            "SIM2 primary logical subscription assignment verified; standby preserved");
    }
    return rc;
}
static int card_get(struct sim_status *out) {
    const uint8_t *body;
    size_t bytes, len;
    int rc = request(11, 0x2f, NULL, 0, &body, &bytes);
    if (rc)
        return rc;
    const uint8_t *v = qmux_tlv(body, bytes, 0x10, &len);
    return v && !sim_decode(v, len, PHYSICAL_SLOT, SUBSCRIPTION, out) ? 0 : -EBADMSG;
}
static int nas_get(struct serving_status *out, const uint8_t **body, size_t *bytes) {
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(3, 0x45, bind, sizeof(bind), body, bytes);
    if (rc)
        return rc;
    rc = request(3, 0x24, NULL, 0, body, bytes);
    return rc ? rc : serving_decode(*body, *bytes, out);
}
static int activate_card(void) {
    struct sim_status status;
    int rc = card_get(&status);
    if (rc)
        return rc;
    if (status.card.card_state != RIL_CARDSTATE_PRESENT)
        return 0;
    int active = status.card.gsm_umts_subscription_app_index;
    if (active >= 0 && active < status.card.num_applications &&
        (status.card.applications[active].app_type == RIL_APPTYPE_SIM ||
         status.card.applications[active].app_type == RIL_APPTYPE_USIM))
        return 0;
    for (int i = 0; i < status.card.num_applications; i++) {
        if (status.card.applications[i].app_type != RIL_APPTYPE_SIM &&
            status.card.applications[i].app_type != RIL_APPTYPE_USIM)
            continue;
        unsigned len = status.aid_bytes[i];
        uint8_t args[42] = {1, 2, 0, GW_SESSION, 1, 0x10, 2 + len, 0, PHYSICAL_SLOT + 1, len};
        if (len)
            memcpy(args + 10, status.raw_aid[i], len);
        const uint8_t *body;
        size_t bytes;
        activation_attempted = 1;
        return request(11, 0x38, args, 10 + len, &body, &bytes);
    }
    return 0;
}
static void completed(RIL_Token token, RIL_Errno error, void *value, size_t bytes) {
    environment->OnRequestComplete(token, error, value, bytes);
}
static void get_card(RIL_Token token) {
    struct sim_status status;
    int rc = card_get(&status);
    completed(token, modem_error(rc), rc ? NULL : &status.card, rc ? 0 : sizeof(status.card));
}
static void baseband(RIL_Token token) {
    const uint8_t *body;
    size_t bytes, len;
    int rc = request(2, 0x23, NULL, 0, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    char version[257];
    if (!v || !len || len >= sizeof(version)) {
        completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
        return;
    }
    size_t n = 0;
    while (n < len && v[n] && v[n] != '\n' && v[n] != '\r') {
        version[n] = v[n] >= 32 && v[n] < 127 ? v[n] : '?';
        n++;
    }
    version[n] = 0;
    completed(token, RIL_E_SUCCESS, version, n + 1);
}
static void registration(RIL_Token token, int packet) {
    const uint8_t *body;
    size_t bytes;
    struct serving_status status;
    int rc = nas_get(&status, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    int reg = status.registration;
    if ((reg == 1 || reg == 5) && (packet ? status.ps : status.cs) != 1)
        reg = 0;
    char state[16], lac[16], cell[16], rat[16];
    snprintf(state, sizeof(state), "%d", reg);
    snprintf(rat, sizeof(rat), "%d", status.technology);
    snprintf(lac, sizeof(lac), "%x", status.lac);
    snprintf(cell, sizeof(cell), "%x", status.cell);
    char *strings[15] = {state, status.has_lac ? lac : NULL, status.has_cell ? cell : NULL, rat};
    if (packet) {
        strings[4] = "0";
        strings[5] = "1";
    }
    completed(token, RIL_E_SUCCESS, strings, (packet ? 6 : 15) * sizeof(char *));
}
static void operator_get(RIL_Token token) {
    const uint8_t *body;
    size_t bytes, len;
    struct serving_status status;
    int rc = nas_get(&status, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    const uint8_t *v = qmux_tlv(body, bytes, 0x12, &len);
    char name[257] = {0}, numeric[8] = {0};
    char *strings[3] = {NULL, NULL, NULL};
    if (v) {
        if (len < 5 || len != 5 + (size_t)v[4] || qmux_read16(v) > 999 ||
            qmux_read16(v + 2) > 999) {
            completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
            return;
        }
        unsigned mcc = qmux_read16(v), mnc = qmux_read16(v + 2);
        int digits = mnc >= 100 ? 3 : 2;
        size_t pcs_len;
        const uint8_t *pcs = qmux_tlv(body, bytes, 0x27, &pcs_len);
        if (pcs && pcs_len == 5 && qmux_read16(pcs) == mcc && qmux_read16(pcs + 2) == mnc)
            digits = pcs[4] ? 3 : 2;
        snprintf(numeric, sizeof(numeric), "%03u%0*u", mcc, digits, mnc);
        for (unsigned i = 0; i < v[4]; i++)
            name[i] = v[5 + i] >= 32 && v[5 + i] < 127 ? v[5 + i] : '?';
        strings[0] = name;
        strings[1] = name;
        strings[2] = numeric;
    }
    completed(token, RIL_E_SUCCESS, strings, sizeof(strings));
}
static void signal_get(RIL_Token token) {
    const uint8_t *body;
    size_t bytes, len;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(3, 0x45, bind, sizeof(bind), &body, &bytes);
    if (!rc)
        rc = request(3, 0x20, NULL, 0, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    if (!v || len != 2) {
        completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
        return;
    }
    RIL_SignalStrength_v10 signal = {0};
    signal.GW_SignalStrength.signalStrength = 99;
    signal.GW_SignalStrength.bitErrorRate = 99;
    signal.CDMA_SignalStrength.dbm = -1;
    signal.CDMA_SignalStrength.ecio = -1;
    signal.EVDO_SignalStrength.dbm = -1;
    signal.EVDO_SignalStrength.ecio = -1;
    signal.EVDO_SignalStrength.signalNoiseRatio = -1;
    signal.LTE_SignalStrength.signalStrength = 99;
    signal.LTE_SignalStrength.rsrp = INT_MAX;
    signal.LTE_SignalStrength.rsrq = INT_MAX;
    signal.LTE_SignalStrength.rssnr = INT_MAX;
    signal.LTE_SignalStrength.cqi = INT_MAX;
    signal.LTE_SignalStrength.timingAdvance = INT_MAX;
    signal.TD_SCDMA_SignalStrength.rscp = INT_MAX;
    int dbm = (int8_t)v[0];
    if ((v[1] == 4 || v[1] == 5) && dbm >= -113 && dbm <= -51)
        signal.GW_SignalStrength.signalStrength = (dbm + 113) / 2;
    completed(token, RIL_E_SUCCESS, &signal, sizeof(signal));
}
static void identity_get(RIL_Token token, int request_code) {
    const uint8_t *body;
    size_t bytes, len;
    int rc = request(2, 0x25, NULL, 0, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    char storage[4][33] = {{0}};
    char *values[4] = {NULL, NULL, NULL, NULL};
    const uint8_t types[] = {0x11, 0x13, 0x10, 0x12};
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *v = qmux_tlv(body, bytes, types[i], &len);
        if (!v)
            continue;
        if (len == 0 || len > 32) {
            completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
            return;
        }
        for (size_t j = 0; j < len; j++)
            if (!((v[j] >= '0' && v[j] <= '9') || (v[j] >= 'a' && v[j] <= 'f') ||
                  (v[j] >= 'A' && v[j] <= 'F'))) {
                completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
                return;
            }
        memcpy(storage[i], v, len);
        values[i] = storage[i];
    }
    if (request_code == RIL_REQUEST_DEVICE_IDENTITY)
        completed(token, RIL_E_SUCCESS, values, sizeof(values));
    else {
        unsigned which = request_code == RIL_REQUEST_GET_IMEISV ? 1 : 0;
        completed(token, values[which] ? RIL_E_SUCCESS : RIL_E_REQUEST_NOT_SUPPORTED, values[which],
                  values[which] ? strlen(values[which]) + 1 : 0);
    }
}
static int read_sim(int command, int file, const char *path, int p1, int p2, int p3,
                    const char *aid_hex, const uint8_t **contents, size_t *length, int *sw1,
                    int *sw2) {
    struct sim_status card;
    int rc = card_get(&card);
    if (rc)
        return rc;
    int app = card.card.gsm_umts_subscription_app_index;
    if (app < 0 || app >= card.card.num_applications)
        return -ENODEV;
    if (aid_hex && *aid_hex) {
        uint8_t supplied[32];
        int n = bytes_from_hex(aid_hex, supplied, sizeof(supplied));
        if (n < 0 || n != card.aid_bytes[app] || memcmp(supplied, card.raw_aid[app], n))
            return -EINVAL;
    }
    uint8_t payload[128];
    int n = uim_file_request(payload, sizeof(payload), SUBSCRIPTION, card.raw_aid[app],
                             card.aid_bytes[app], command, file, path, p1, p2, p3);
    if (n < 0)
        return n;
    const uint8_t *body = NULL;
    size_t bytes = 0;
    rc = request(11, command == 0xb0 ? 0x20 : command == 0xb2 ? 0x21 : 0x24, payload, n, &body,
                 &bytes);
    if (rc < 0)
        return rc;
    if (rc > 0) {
        size_t sw_bytes;
        const uint8_t *sw = qmux_tlv(body, bytes, 0x10, &sw_bytes);
        if (!sw || sw_bytes != 2 || (sw[0] == 0x90 && sw[1] == 0))
            return rc;
    }
    if (uim_file_response(body, bytes, command, file, contents, length, sw1, sw2))
        return rc ? rc : -EBADMSG;
    return 0;
}
static void sim_io(void *data, size_t bytes, RIL_Token token) {
    if (!data || bytes != sizeof(RIL_SIM_IO_v6)) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    RIL_SIM_IO_v6 *io = data;
    if (io->pin2 && *io->pin2) {
        completed(token, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    if (io->command != 0xb0 && io->command != 0xb2 && io->command != 0xc0) {
        completed(token, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    const uint8_t *contents = NULL;
    size_t len = 0;
    RIL_SIM_IO_Response result = {0};
    int rc = read_sim(io->command, io->fileid, io->path, io->p1, io->p2, io->p3, io->aidPtr,
                      &contents, &len, &result.sw1, &result.sw2);
    if (rc) {
        fprintf(stderr, "RIL SIM IO command=%x file=%x p1=%d p2=%d p3=%d error=%d\n", io->command,
                io->fileid, io->p1, io->p2, io->p3, rc);
        completed(token, rc == -EINVAL ? RIL_E_INVALID_ARGUMENTS : modem_error(rc), NULL, 0);
        return;
    }
    char hex[1025];
    bytes_to_hex(contents, len, hex);
    result.simResponse = hex;
    completed(token, RIL_E_SUCCESS, &result, sizeof(result));
}
static void imsi_get(void *data, size_t bytes, RIL_Token token) {
    const char *aid = NULL;
    if (data) {
        if (bytes != sizeof(char *)) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        aid = ((char **)data)[0];
        if (aid && strnlen(aid, 65) > 64) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
    } else if (bytes) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    const uint8_t *contents = NULL;
    size_t len = 0;
    int sw1, sw2;
    int rc = read_sim(0xb0, 0x6f07, "3F007F20", 0, 0, 9, aid, &contents, &len, &sw1, &sw2);
    char imsi[16];
    if (rc || sw1 != 0x90 || sw2 || imsi_decode(contents, len, imsi) < 0) {
        completed(token, rc ? modem_error(rc) : RIL_E_SIM_ERR, NULL, 0);
        return;
    }
    completed(token, RIL_E_SUCCESS, imsi, strlen(imsi) + 1);
    memset(imsi, 0, sizeof(imsi));
}
static void data_shutdown(void);
static void power_set(void *data, size_t bytes, RIL_Token token) {
    if (!data || bytes != sizeof(int) || (*(int *)data != 0 && *(int *)data != 1)) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    int on = *(int *)data;
    if (!on)
        data_shutdown();
    activation_attempted = 0;
    int rc = mode_set(on ? 0 : 1);
    if (!rc)
        atomic_store(&radio_state, on ? RADIO_STATE_ON : RADIO_STATE_OFF);
    if (!rc && on) {
        int sim_rc = activate_card();
        if (sim_rc)
            fprintf(stderr, "RIL SIM session activation error=%d\n", sim_rc);
    }
    completed(token, modem_error(rc), NULL, 0);
    environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
}
static int radio_modes(void) {
    const uint8_t *body;
    size_t bytes, len;
    int rc = request(2, 0x20, NULL, 0, &body, &bytes);
    if (rc)
        return -1;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    if (!v || len < 11 || len != 11 + (size_t)v[10])
        return -1;
    int modes = 0;
    for (unsigned i = 0; i < v[10]; i++) {
        if (v[11 + i] == 4)
            modes |= 4;
        if (v[11 + i] == 5)
            modes |= 8;
    }
    return modes ? modes : -1;
}
static void capabilities_get(RIL_Token token) {
    int modes = radio_modes();
    if (modes < 0) {
        completed(token, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }
    RIL_RadioCapability cap = {.version = RIL_RADIO_CAPABILITY_VERSION,
                               .phase = RC_PHASE_CONFIGURED,
                               .status = RC_STATUS_SUCCESS};
    if (modes & 4)
        cap.rat |= RAF_GSM | RAF_GPRS | RAF_EDGE;
    if (modes & 8)
        cap.rat |= RAF_UMTS;
    snprintf(cap.logicalModemUuid, sizeof(cap.logicalModemUuid), "superman.msm8226.sim2");
    completed(token, RIL_E_SUCCESS, &cap, sizeof(cap));
}
static int preferred_mode(void) {
    const uint8_t *body;
    size_t bytes, len;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int supported = radio_modes();
    if (supported < 0)
        return -1;
    if (request(3, 0x45, bind, sizeof(bind), &body, &bytes) ||
        request(3, 0x34, NULL, 0, &body, &bytes))
        return -1;
    const uint8_t *v = qmux_tlv(body, bytes, 0x11, &len);
    if (!v || len != 2)
        return -1;
    int mode = qmux_read16(v) & supported;
    if (mode == 4)
        return PREF_NET_TYPE_GSM_ONLY;
    if (mode == 8)
        return PREF_NET_TYPE_WCDMA;
    if (mode != 12)
        return -1;
    v = qmux_tlv(body, bytes, 0x19, &len);
    return v && len == 4 && v[0] == 2 && v[1] == 0 && v[2] == 0 && v[3] == 0
               ? PREF_NET_TYPE_GSM_WCDMA
               : PREF_NET_TYPE_GSM_WCDMA_AUTO;
}
static void preference_get(RIL_Token token) {
    int mode = preferred_mode();
    completed(token, mode < 0 ? RIL_E_MODEM_ERR : RIL_E_SUCCESS, mode < 0 ? NULL : &mode,
              mode < 0 ? 0 : sizeof(mode));
}
static void preference_set(void *data, size_t bytes, RIL_Token token) {
    if (!data || bytes != sizeof(int)) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    int mode = *(int *)data;
    if (mode < 0 || mode > 3) {
        completed(token, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
        return;
    }
    int mask = mode == 1 ? 4 : mode == 2 ? 8 : 12, supported = radio_modes();
    if (supported < 0) {
        completed(token, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }
    if ((supported & mask) != mask) {
        completed(token, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
        return;
    }
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    const uint8_t *body;
    size_t body_bytes;
    int rc = request(3, 0x45, bind, sizeof(bind), &body, &body_bytes);
    // Duration 0 is until power cycle. Android retains the user's preference.
    uint8_t change[] = {0x11, 2, 0, mask, 0, 0x17, 1, 0, 0, 0x19, 4, 0, mode == 0 ? 2 : 0, 0, 0, 0};
    if (!rc)
        rc = request(3, 0x33, change, sizeof(change), &body, &body_bytes);
    if (!rc) {
        struct timespec begin, now;
        if (clock_gettime(CLOCK_MONOTONIC, &begin))
            rc = -EIO;
        while (!rc) {
            int readback = preferred_mode();
            if (readback == mode)
                break;
            if (readback < 0 || clock_gettime(CLOCK_MONOTONIC, &now) ||
                now.tv_sec - begin.tv_sec >= 5) {
                rc = -EIO;
                break;
            }
            usleep(100000);
        }
    }
    completed(token, modem_error(rc), NULL, 0);
}
static void radio_technology(RIL_Token token) {
    const uint8_t *body;
    size_t bytes;
    struct serving_status status;
    int rc = nas_get(&status, &body, &bytes);
    int tech = rc ? RADIO_TECH_UNKNOWN : status.technology;
    completed(token, modem_error(rc), rc ? NULL : &tech, rc ? 0 : sizeof(tech));
}
static int calls_get(struct call_list *calls) {
    const uint8_t *body;
    size_t bytes;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(9, 0x44, bind, sizeof(bind), &body, &bytes);
    if (!rc)
        rc = request(9, 0x2f, NULL, 0, &body, &bytes);
    return rc ? rc : calls_decode(body, bytes, calls);
}
static void current_calls(RIL_Token token) {
    struct call_list calls;
    int rc = calls_get(&calls);
    completed(token, modem_error(rc), rc ? NULL : calls.pointers,
              rc ? 0 : calls.count * sizeof(RIL_Call *));
}
static void call_control(int code, void *data, size_t bytes, RIL_Token token) {
    uint8_t payload[84];
    int length = 0, message;
    if (code == RIL_REQUEST_DIAL) {
        if (!data || bytes != sizeof(RIL_Dial)) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        RIL_Dial *dial = data;
        if (dial->clir || dial->uusInfo) {
            completed(token, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            return;
        }
        length = dial_request(payload, sizeof(payload), dial->address);
        if (length < 0) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        message = 0x20;
    } else {
        if ((code == RIL_REQUEST_HANGUP && (!data || bytes != sizeof(int))) ||
            (code == RIL_REQUEST_ANSWER && bytes)) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        struct call_list calls;
        int rc = calls_get(&calls);
        if (rc) {
            completed(token, modem_error(rc), NULL, 0);
            return;
        }
        int id = 0;
        for (unsigned i = 0; i < calls.count; i++)
            if (code == RIL_REQUEST_HANGUP ? calls.calls[i].index == *(int *)data
                                           : calls.calls[i].state == RIL_CALL_INCOMING) {
                id = calls.calls[i].index;
                break;
            }
        if (!id) {
            completed(token, RIL_E_INVALID_STATE, NULL, 0);
            return;
        }
        payload[0] = 1;
        payload[1] = 1;
        payload[2] = 0;
        payload[3] = id;
        length = 4;
        message = code == RIL_REQUEST_HANGUP ? 0x21 : 0x22;
    }
    const uint8_t *body;
    size_t body_bytes;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(9, 0x44, bind, sizeof(bind), &body, &body_bytes);
    if (!rc)
        rc = request(9, message, payload, length, &body, &body_bytes);
    if (!rc && message == 0x20) {
        size_t n;
        const uint8_t *id = qmux_tlv(body, body_bytes, 0x10, &n);
        if (!id || n != 1 || !id[0])
            rc = -EBADMSG;
    }
    completed(token, modem_error(rc), NULL, 0);
    if (!rc)
        environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
}
static void selection_mode(RIL_Token token) {
    const uint8_t *body;
    size_t bytes, len;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(3, 0x45, bind, sizeof(bind), &body, &bytes);
    if (!rc)
        rc = request(3, 0x34, NULL, 0, &body, &bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    const uint8_t *v = qmux_tlv(body, bytes, 0x16, &len);
    if (!v || len != 1 || v[0] > 1) {
        completed(token, RIL_E_INVALID_RESPONSE, NULL, 0);
        return;
    }
    int mode = v[0];
    completed(token, RIL_E_SUCCESS, &mode, sizeof(mode));
}
static void messaging_stop(void) {
    const uint8_t *body;
    size_t bytes;
    messaging_ready = 0;
    if (messaging_primary) {
        uint8_t report[] = {0x10, 1, 0, 0}, primary[] = {1, 1, 0, 0};
        int a = request(5, 1, report, sizeof(report), &body, &bytes);
        int b = request(5, 0x45, primary, sizeof(primary), &body, &bytes);
        if (a || b)
            __android_log_print(ANDROID_LOG_WARN, "SupermanRIL", "WMS client cleanup=%d/%d", a, b);
        messaging_primary = 0;
    }
    memset(&incoming_sms, 0, sizeof(incoming_sms));
}
static void messaging_indication(uint8_t service, uint16_t message, const uint8_t *body,
                                 size_t bytes) {
    if (service != 5 || message != 1 || !messaging_ready)
        return;
    struct sms_event event;
    int rc = sms_event_decode(body, bytes, &event);
    if (rc == 1)
        rc = sms_queue_push(&incoming_sms, &event);
    if (rc < 0)
        __android_log_print(ANDROID_LOG_WARN, "SupermanRIL",
                            "WMS receive rejected=%d (payload private)", rc);
}
static int messaging_start(void) {
    const uint8_t *body;
    size_t bytes, n;
    uint8_t bind[] = {1, 1, 0, SUBSCRIPTION};
    int rc = request(5, 0x4c, bind, sizeof(bind), &body, &bytes);
    if (rc)
        return rc;
    rc = request(5, 0x33, NULL, 0, &body, &bytes);
    if (rc)
        return rc;
    rc = sms_routes_transfer(body, bytes);
    if (rc)
        return rc;
    const uint8_t *status=qmux_tlv(body,bytes,0x10,&n);
    if(!status||n!=1||*status>1)return -EBADMSG;
    int reports=*status;
    /* Keep all stock routes: this firmware did not read back a cleared
     * transfer-report flag, so changing it cannot pass cleanup qualification. */
    messaging_ready=1;
    uint8_t primary[] = {1, 1, 0, 1}, report[] = {0x10, 1, 0, 1};
    messaging_primary = 1;
    rc = request(5, 0x45, primary, sizeof(primary), &body, &bytes);
    if (rc)
        goto fail;
    rc = request(5, 1, report, sizeof(report), &body, &bytes);
    if (rc)
        goto fail;
    messaging_ready = 1;
    __android_log_print(ANDROID_LOG_INFO, "SupermanRIL",
                        "WMS client ready subscription=%u status_reports=%d; network service separately qualified",
                        SUBSCRIPTION,reports);
    return 0;
fail:
    messaging_stop();
    return rc;
}
/* Publish outside the transport lock: Android may synchronously request an ACK. */
static void messaging_publish(void) {
    for (unsigned i = 0; i < SUPERMAN_SMS_QUEUE; i++) {
        pthread_mutex_lock(&lock);
        if (!messaging_ready || !incoming_sms.count || incoming_sms.delivered) {
            pthread_mutex_unlock(&lock);
            return;
        }
        struct sms_event event = incoming_sms.events[incoming_sms.head];
        if (event.sim_index >= 0)
            sms_queue_pop(&incoming_sms);
        else
            incoming_sms.delivered = 1;
        pthread_mutex_unlock(&lock);
        if (event.sim_index >= 0)
            environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM, &event.sim_index,
                                               sizeof(event.sim_index));
        else
            environment->OnUnsolicitedResponse(event.status_report
                                                   ? RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT
                                                   : RIL_UNSOL_RESPONSE_NEW_SMS,
                                               event.pdu, strlen(event.pdu));
    }
}
static RIL_Errno messaging_error(int rc) {
    if (rc == 52)
        return RIL_E_INVALID_MODEM_STATE;
    return modem_error(rc);
}
static void messaging_request(int code, void *data, size_t size, RIL_Token token) {
    uint8_t payload[SUPERMAN_SMS_MAX_PDU + 16];
    int n = 0, rc;
    unsigned message;
    const uint8_t *body;
    size_t bytes;
    if (code == RIL_REQUEST_SEND_SMS || code == RIL_REQUEST_SEND_SMS_EXPECT_MORE) {
        if (!data || size != 2 * sizeof(char *)) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        char **args = data;
        n = sms_send_request(payload, sizeof(payload), args[0], args[1]);
        if (n < 0) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        message = 0x20;
    } else if (code == RIL_REQUEST_SMS_ACKNOWLEDGE) {
        if (!data || size != 2 * sizeof(int) || (*(int *)data != 0 && *(int *)data != 1) ||
            ((int *)data)[1] < 0 || ((int *)data)[1] > 255) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        if (!incoming_sms.count || !incoming_sms.delivered) {
            completed(token, RIL_E_INVALID_STATE, NULL, 0);
            return;
        }
        struct sms_event *event = &incoming_sms.events[incoming_sms.head];
        if (!event->ack_required) {
            sms_queue_pop(&incoming_sms);
            completed(token, RIL_E_SUCCESS, NULL, 0);
            return;
        }
        n = sms_ack_request(payload, sizeof(payload), event, ((int *)data)[0], ((int *)data)[1]);
        if (n < 0) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        message = 0x37;
    } else if (code == RIL_REQUEST_REPORT_SMS_MEMORY_STATUS) {
        if (!data || size != sizeof(int) || (*(int *)data != 0 && *(int *)data != 1)) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        memcpy(payload, (uint8_t[]){1, 1, 0, *(int *)data}, 4);
        n = 4;
        message = 0x3b;
    } else {
        if (data || size) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        message = 0x34;
    }
    if (!messaging_ready) {
        completed(token, RIL_E_INVALID_MODEM_STATE, NULL, 0);
        return;
    }
    rc = request(5, message, payload, n, &body, &bytes);
    /* Android does not retry ACK requests; release this transaction even on an
     * explicit failure so a modem retry/new message cannot wedge the queue. */
    if (code == RIL_REQUEST_SMS_ACKNOWLEDGE)
        sms_queue_pop(&incoming_sms);
    if (rc) {
        completed(token, messaging_error(rc), NULL, 0);
        return;
    }
    if (message == 0x20) {
        RIL_SMS_Response result = {.messageRef = 0, .ackPDU = NULL, .errorCode = 0};
        rc = sms_send_response(body, bytes, &result.messageRef);
        completed(token, rc ? RIL_E_INVALID_RESPONSE : RIL_E_SUCCESS, rc ? NULL : &result,
                  rc ? 0 : sizeof(result));
    } else if (message == 0x34) {
        char address[35];
        rc = sms_smsc_response(body, bytes, address, sizeof(address));
        completed(token, rc ? RIL_E_INVALID_RESPONSE : RIL_E_SUCCESS, rc ? NULL : address,
                  rc ? 0 : strlen(address) + 1);
    } else
        completed(token, RIL_E_SUCCESS, NULL, 0);
}
#include "data-ril.h"
static void radio_indication(uint8_t service,uint16_t message,const uint8_t *body,size_t bytes) {
    messaging_indication(service,message,body,bytes);
    data_indication(service,message,body,bytes);
}
static int supports(int code) {
    switch (code) {
    case RIL_REQUEST_SETUP_DATA_CALL:
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
    case RIL_REQUEST_DATA_CALL_LIST:

    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
    case RIL_REQUEST_GET_SMSC_ADDRESS:
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:

    case RIL_REQUEST_DIAL:
    case RIL_REQUEST_HANGUP:
    case RIL_REQUEST_ANSWER:
    case RIL_REQUEST_GET_CURRENT_CALLS:
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
    case RIL_REQUEST_GET_RADIO_CAPABILITY:
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_GET_SIM_STATUS:
    case RIL_REQUEST_SIM_IO:
    case RIL_REQUEST_GET_IMSI:
    case RIL_REQUEST_GET_IMEI:
    case RIL_REQUEST_GET_IMEISV:
    case RIL_REQUEST_DEVICE_IDENTITY:
    case RIL_REQUEST_BASEBAND_VERSION:
    case RIL_REQUEST_RADIO_POWER:
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
    case RIL_REQUEST_OPERATOR:
    case RIL_REQUEST_SIGNAL_STRENGTH:
    case RIL_REQUEST_VOICE_RADIO_TECH:
        return 1;
    default:
        return 0;
    }
}
static void on_request(int code, void *data, size_t bytes, RIL_Token token) {
    if (!supports(code)) {
        completed(token, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    pthread_mutex_lock(&lock);
    if (transport.fd < 0) {
        completed(token, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        pthread_mutex_unlock(&lock);
        return;
    }
    switch (code) {
    case RIL_REQUEST_SETUP_DATA_CALL:
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
    case RIL_REQUEST_DATA_CALL_LIST:
        data_request(code,data,bytes,token);
        break;
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
    case RIL_REQUEST_GET_SMSC_ADDRESS:
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
        messaging_request(code,data,bytes,token);
        break;
    case RIL_REQUEST_DIAL:
    case RIL_REQUEST_HANGUP:
    case RIL_REQUEST_ANSWER:
        call_control(code, data, bytes, token);
        break;
    case RIL_REQUEST_GET_CURRENT_CALLS:
        current_calls(token);
        break;
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        selection_mode(token);
        break;

    case RIL_REQUEST_GET_RADIO_CAPABILITY:
        capabilities_get(token);
        break;
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        preference_get(token);
        break;
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        preference_set(data, bytes, token);
        break;
    case RIL_REQUEST_SIM_IO:
        sim_io(data, bytes, token);
        break;
    case RIL_REQUEST_GET_IMSI:
        imsi_get(data, bytes, token);
        break;
    case RIL_REQUEST_GET_IMEI:
    case RIL_REQUEST_GET_IMEISV:
    case RIL_REQUEST_DEVICE_IDENTITY:
        identity_get(token, code);
        break;
    case RIL_REQUEST_GET_SIM_STATUS:
        get_card(token);
        break;
    case RIL_REQUEST_BASEBAND_VERSION:
        baseband(token);
        break;
    case RIL_REQUEST_RADIO_POWER:
        power_set(data, bytes, token);
        break;
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        registration(token, 0);
        break;
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
        registration(token, 1);
        break;
    case RIL_REQUEST_OPERATOR:
        operator_get(token);
        break;
    case RIL_REQUEST_SIGNAL_STRENGTH:
        signal_get(token);
        break;
    case RIL_REQUEST_VOICE_RADIO_TECH:
        radio_technology(token);
        break;
    }
    pthread_mutex_unlock(&lock);
}
static RIL_RadioState state_get(void) { return (RIL_RadioState)atomic_load(&radio_state); }
static void cancelled(RIL_Token token) { data_cancel(token); }
static const char *version(void) { return "Superman QMUX RIL PF30"; }
static void *monitor(void *unused) {
    (void)unused;
    // Register HIDL before PhoneFactory waits for it; open QMUX only after guarded modem startup.
    for (;;) {
        pthread_mutex_lock(&lock);
        int stop = stopping;
        pthread_mutex_unlock(&lock);
        if (terminate_requested)
            _exit(0);
        if (stop)
            return NULL;
        char state[PROP_VALUE_MAX] = {0};
        __system_property_get("vendor.superman.modem.state", state);
        if (!strcmp(state, "transport_ready"))
            break;
        sleep(1);
    }
    pthread_mutex_lock(&lock);
    int rc = qmux_open(&transport);
    if (!rc) {
        transport.indication=radio_indication;
        rc = select_subscription();
        if (rc)
            qmux_close(&transport);
    }
    pthread_mutex_unlock(&lock);
    if (rc) {
        __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL", "QMUX startup error=%d", rc);
        _exit(1);
    }
    for (;;) {
        int radio_changed = 0, card_changed = 0, network_changed = 0, calls_changed = 0;
        pthread_mutex_lock(&lock);
        if (stopping || terminate_requested) {
            data_shutdown();
            messaging_stop();
            qmux_close(&transport);
            pthread_mutex_unlock(&lock);
            if (terminate_requested)
                _exit(0);
            break;
        }
        int mode = mode_get();
        int new_state =
            mode < 0 ? RADIO_STATE_UNAVAILABLE : mode == 0 ? RADIO_STATE_ON : RADIO_STATE_OFF;
        radio_changed = new_state != atomic_load(&radio_state);
        atomic_store(&radio_state, new_state);
        const uint8_t *body;
        size_t bytes, len;
        if (!request(11, 0x2f, NULL, 0, &body, &bytes)) {
            const uint8_t *v = qmux_tlv(body, bytes, 0x10, &len);
            struct sim_status parsed;
            if (v && len <= sizeof(previous_card) &&
                !sim_decode(v, len, PHYSICAL_SLOT, SUBSCRIPTION, &parsed)) {
                if (len != previous_card_bytes || memcmp(previous_card, v, len)) {
                    memcpy(previous_card, v, len);
                    previous_card_bytes = len;
                    card_changed = 1;
                }
                int app = parsed.card.gsm_umts_subscription_app_index;
                if (!messaging_attempted && new_state == RADIO_STATE_ON && app >= 0 &&
                    app < parsed.card.num_applications &&
                    parsed.card.applications[app].app_state == RIL_APPSTATE_READY) {
                    messaging_attempted = 1;
                    int sms_rc = messaging_start();
                    if (sms_rc)
                        __android_log_print(ANDROID_LOG_WARN, "SupermanRIL",
                                            "WMS startup failed=%d; other radio APIs retained",
                                            sms_rc);
                }
                if (parsed.card.card_state != RIL_CARDSTATE_PRESENT)
                    activation_attempted = 0;
                else if (new_state == RADIO_STATE_ON && parsed.provisioning_index == 0xffff &&
                         !activation_attempted)
                    (void)activate_card();
            }
        }
        struct serving_status status;
        if (!nas_get(&status, &body, &bytes)) {
            network_changed =
                !previous_serving_valid || memcmp(&status, &previous_serving, sizeof(status));
            previous_serving = status;
            previous_serving_valid = 1;
        }
        struct call_list calls;
        if (!calls_get(&calls)) {
            struct call_snapshot snapshot[SUPERMAN_MAX_CALLS] = {0};
            for (unsigned i = 0; i < calls.count; i++) {
                RIL_Call *c = &calls.calls[i];
                snapshot[i].id = c->index;
                snapshot[i].state = c->state;
                snapshot[i].incoming = c->isMT;
                snapshot[i].multiparty = c->isMpty;
                snapshot[i].presentation = c->numberPresentation;
                if (c->number)
                    snprintf(snapshot[i].number, sizeof(snapshot[i].number), "%s", c->number);
            }
            calls_changed = !previous_calls_valid || calls.count != previous_call_count ||
                            memcmp(previous_calls, snapshot, sizeof(snapshot));
            memcpy(previous_calls, snapshot, sizeof(snapshot));
            previous_call_count = calls.count;
            previous_calls_valid = 1;
        }
        (void)data_refresh();
        data_process();
        pthread_mutex_unlock(&lock);
        if (calls_changed)
            environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
        if (radio_changed)
            environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
        if (card_changed)
            environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        if (network_changed)
            environment->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL,
                                               0);
        messaging_publish();data_publish();
        for (unsigned i = 0; i < 5 && !terminate_requested; i++) {
            sleep(1);
            pthread_mutex_lock(&lock);
            if (transport.fd>=0) {
                int poll_rc = qmux_poll(&transport);
                if (poll_rc)
                    __android_log_print(ANDROID_LOG_WARN, "SupermanRIL", "QMUX indication poll=%d",
                                        poll_rc);
            }
            data_process();
            pthread_mutex_unlock(&lock);
            messaging_publish();data_publish();
        }
    }
    return NULL;
}
static const RIL_RadioFunctions functions = {RIL_VERSION, on_request, state_get,
                                             supports,    cancelled,  version};
const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!env || !env->OnRequestComplete || !env->OnUnsolicitedResponse)
        return NULL;
    if (started)
        return NULL;
    environment = env;
    struct sigaction action = {0};
    action.sa_handler = terminate_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGHUP, &action, NULL))
        return NULL;
    if (pthread_create(&thread, NULL, monitor, NULL))
        return NULL;
    started = 1;
    return &functions;
}

// Explicit lifecycle hook used by the native harness and the owned service loader.
void superman_ril_shutdown(void) {
    if (!started)
        return;
    pthread_mutex_lock(&lock);
    stopping = 1;
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    started = 0;
}

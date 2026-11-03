// SPDX-License-Identifier: Apache-2.0
/* Included once by superman-ril.c; callers serialize QMUX and state with lock. */
#include "data-call.h"
#include "data-link.h"
enum data_phase { DATA_IDLE, DATA_STARTING, DATA_ACTIVE, DATA_RETAINED };
static struct {
    enum data_phase phase;
    int client_owned, setup_pending, response_ready, response_result;
    int disconnected, link_active, notify, configured, status_seen, reconfigure;
    RIL_Token token;
    int64_t deadline;
    uint32_t handle;
    uint8_t response[1024];
    size_t response_bytes;
    struct data_profile profile;
    struct data_lease lease;
    struct data_link link;
} packet_data = {.link = {.fd = -1}};
static _Atomic(RIL_Token) cancellable_data_token;
struct data_response_strings {
    char address[20], dns[34], gateway[16];
};
static int data_visible(void) {
    return packet_data.configured && packet_data.handle && packet_data.link.address_owned &&
           !packet_data.disconnected;
}
static void data_response(RIL_Data_Call_Response_v11 *response,
                          struct data_response_strings *strings) {
    char ip[16], dns0[16], dns1[16];
    data_ipv4_string(packet_data.lease.address, ip);
    snprintf(strings->address, sizeof(strings->address), "%s/%u", ip, packet_data.lease.prefix);
    strings->dns[0] = 0;
    if (packet_data.lease.dns[0]) {
        data_ipv4_string(packet_data.lease.dns[0], dns0);
        snprintf(strings->dns, sizeof(strings->dns), "%s", dns0);
    }
    if (packet_data.lease.dns[1]) {
        data_ipv4_string(packet_data.lease.dns[1], dns1);
        size_t used = strlen(strings->dns);
        snprintf(strings->dns + used, sizeof(strings->dns) - used, "%s%s", used ? " " : "", dns1);
    }
    data_ipv4_string(packet_data.lease.gateway, strings->gateway);
    *response = (RIL_Data_Call_Response_v11){.status = 0,
                                             .suggestedRetryTime = -1,
                                             .cid = 1,
                                             .active = packet_data.link_active ? 2 : 1,
                                             .type = "IP",
                                             .ifname = packet_data.link.name,
                                             .addresses = strings->address,
                                             .dnses = strings->dns,
                                             .gateways = strings->gateway,
                                             .pcscf = "",
                                             .mtu = packet_data.lease.mtu};
}
static int data_cleanup(void);
static void data_complete(RIL_Errno error) {
    if (!packet_data.setup_pending)
        return;
    /* Cancellation can arrive while settings/configuration holds the radio lock. */
    RIL_Token claimed = atomic_exchange(&cancellable_data_token, NULL);
    if (error == RIL_E_SUCCESS && !claimed) {
        (void)data_cleanup();
        error = RIL_E_CANCELLED;
    }
    RIL_Token token = packet_data.token;
    packet_data.setup_pending = 0;
    packet_data.token = NULL;
    RIL_Data_Call_Response_v11 response;
    struct data_response_strings strings;
    if (error == RIL_E_SUCCESS)
        data_response(&response, &strings);
    completed(token, error, error == RIL_E_SUCCESS ? &response : NULL,
              error == RIL_E_SUCCESS ? sizeof(response) : 0);
}
static void data_async_response(int result, const uint8_t *body, size_t bytes) {
    /* Runs inside the transport read: only copy; no nested modem operations. */
    packet_data.response_result = result;
    packet_data.response_bytes = 0;
    if (bytes > sizeof(packet_data.response))
        packet_data.response_result = -EBADMSG;
    else {
        memcpy(packet_data.response, body, bytes);
        packet_data.response_bytes = bytes;
    }
    packet_data.response_ready = 1;
}
static void data_indication(uint8_t service, uint16_t message, const uint8_t *body, size_t bytes) {
    if (service != 1 || message != 0x22 || !packet_data.client_owned)
        return;
    int state = data_packet_status(body, bytes);
    if (state < 0)
        return;
    packet_data.status_seen = 1;
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &n);
    if (v && n == 2 && v[1]) {
        packet_data.reconfigure = 1;
        packet_data.configured = 0;
        packet_data.notify = 1;
    }
    if (state == 1)
        packet_data.disconnected = 1;
    if (state == 2 || state == 3) {
        int active = state == 2;
        if (packet_data.link_active != active && data_visible())
            packet_data.notify = 1;
        packet_data.link_active = active;
    }
}
static void data_take_response(void) {
    if (!packet_data.response_ready)
        return;
    if (!packet_data.response_result) {
        uint32_t handle;
        int rc = data_handle_decode(packet_data.response, packet_data.response_bytes, &handle);
        if (rc)
            packet_data.response_result = rc;
        else {
            packet_data.handle = handle;
            if (!packet_data.status_seen)
                packet_data.link_active = 1;
        }
    }
}
static int data_status_get(void) {
    const uint8_t *body;
    size_t bytes;
    if (!packet_data.client_owned || !transport.clients[1])
        return -ENODEV;
    int rc = request(1, 0x22, NULL, 0, &body, &bytes);
    return rc ? -EIO : data_packet_status(body, bytes);
}
static int data_cleanup(void) {
    const uint8_t *body;
    size_t bytes;
    int was_visible = data_visible();
    if (packet_data.client_owned && transport.async_callback == data_async_response &&
        transport.async_service == 1 && transport.async_client == transport.clients[1]) {
        uint8_t abort[] = {1, 2, 0, 0, 0};
        qmux_write16(abort + 3, transport.async_transaction);
        (void)request(1, 2, abort, sizeof(abort), &body, &bytes);
    }
    data_take_response();
    packet_data.response_ready = 0;
    if (packet_data.handle && !packet_data.disconnected) {
        uint8_t stop[] = {1, 4, 0, 0, 0, 0, 0};
        for (unsigned i = 0; i < 4; i++)
            stop[3 + i] = packet_data.handle >> (8 * i);
        int stop_rc = request(1, 0x21, stop, sizeof(stop), &body, &bytes);
        int64_t start = monotonic_ms();
        int state = data_status_get();
        while (state > 1 && start >= 0 && monotonic_ms() - start < 2000) {
            usleep(100000);
            state = data_status_get();
        }
        if (state != 1) {
            packet_data.phase = DATA_RETAINED;
            return stop_rc ? -EIO : state < 0 ? state : -ETIMEDOUT;
        }
        packet_data.disconnected = 1;
    }
    if (packet_data.disconnected) {
        if(packet_data.configured)packet_data.notify=1;
        packet_data.handle = 0;
    }
    if (packet_data.client_owned) {
        int rc = qmux_release_client(&transport, 1);
        if (rc) {
            packet_data.phase = DATA_RETAINED;
            return rc < 0 ? rc : -EIO;
        }
        packet_data.client_owned = 0;
    }
    int rc = data_link_restore(&packet_data.link);
    if (rc) {
        packet_data.phase = DATA_RETAINED;
        return rc;
    }
    packet_data.handle = 0;
    packet_data.phase = DATA_IDLE;
    packet_data.response_ready = packet_data.disconnected = packet_data.link_active = 0;
    packet_data.configured = packet_data.status_seen = packet_data.reconfigure = 0;
    memset(&packet_data.profile, 0, sizeof(packet_data.profile));
    memset(&packet_data.lease, 0, sizeof(packet_data.lease));
    if (was_visible)
        packet_data.notify = 1;
    return 0;
}
static void data_process(void) {
    if (packet_data.phase == DATA_STARTING) {
        int cancelled = atomic_load(&cancellable_data_token) == NULL;
        int64_t now = monotonic_ms();
        if (cancelled || now < 0 || now >= packet_data.deadline) {
            int rc = data_cleanup();
            data_complete(cancelled ? RIL_E_CANCELLED
                                    : now < 0 ? RIL_E_SYSTEM_ERR : RIL_E_NETWORK_ERR);
            if (rc)
                __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL", "Data cleanup retained=%d",
                                    rc);
            return;
        }
        if (!packet_data.response_ready)
            return;
        data_take_response();
        if (packet_data.response_result) {
            RIL_Errno error = modem_error(packet_data.response_result);
            int rc = data_cleanup();
            data_complete(error);
            if (rc)
                __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL",
                                    "Data failure cleanup retained=%d", rc);
            return;
        }
        const uint8_t *body;
        size_t bytes;
        uint8_t settings[] = {0x10, 4, 0, 0x10, 0x23, 0, 0};
        int rc = request(1, 0x2d, settings, sizeof(settings), &body, &bytes);
        RIL_Errno error = modem_error(rc);
        if (!rc) {
            rc = data_settings_decode(body, bytes, &packet_data.lease);
            if (rc)
                error = RIL_E_INVALID_RESPONSE;
        }
        if (!rc && packet_data.disconnected) {
            rc = -ENOTCONN;
            error = RIL_E_NETWORK_ERR;
        }
        if (!rc) {
            rc = data_link_apply(&packet_data.link, &packet_data.lease);
            if (rc)
                error = RIL_E_SYSTEM_ERR;
        }
        if (rc) {
            int cleanup = data_cleanup();
            data_complete(error);
            if (cleanup)
                __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL",
                                    "Data configure cleanup retained=%d", cleanup);
            return;
        }
        packet_data.phase = DATA_ACTIVE;
        packet_data.configured = 1;
        packet_data.response_ready = 0;
        packet_data.notify = 1;
        data_complete(RIL_E_SUCCESS);
    } else if (packet_data.phase == DATA_ACTIVE &&
               (packet_data.disconnected || packet_data.reconfigure)) {
        int rc = data_cleanup();
        packet_data.notify = 1;
        if (rc)
            __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL",
                                "Data disconnect cleanup retained=%d", rc);
    }
}
static int data_refresh(void) {
    if (packet_data.phase != DATA_ACTIVE && packet_data.phase != DATA_RETAINED)
        return 0;
    if (!packet_data.client_owned)
        return packet_data.disconnected ? 0 : -ENODEV;
    int state = data_status_get();
    if (state < 0)
        return state;
    if (state == 1)
        packet_data.disconnected = 1;
    else if (state == 2 || state == 3) {
        if (packet_data.link_active != (state == 2))
            packet_data.notify = 1;
        packet_data.link_active = state == 2;
    }
    data_process();
    return 0;
}
static void data_setup(void *args, size_t bytes, RIL_Token token) {
    struct data_profile profile;
    int rc = data_profile_parse(args, bytes, &profile);
    if (rc || !token) {
        completed(token, rc == -ENOTSUP ? RIL_E_MODE_NOT_SUPPORTED : RIL_E_INVALID_ARGUMENTS, NULL,
                  0);
        return;
    }
    rc = data_refresh();
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    if (packet_data.phase == DATA_ACTIVE && !strcmp(profile.apn, packet_data.profile.apn) &&
        !strcmp(profile.user, packet_data.profile.user) &&
        !strcmp(profile.password, packet_data.profile.password) &&
        profile.authentication == packet_data.profile.authentication) {
        RIL_Data_Call_Response_v11 response;
        struct data_response_strings strings;
        data_response(&response, &strings);
        completed(token, RIL_E_SUCCESS, &response, sizeof(response));
        return;
    }
    if (packet_data.phase != DATA_IDLE || transport.clients[1]) {
        completed(token, RIL_E_NO_RESOURCES, NULL, 0);
        return;
    }
    if (atomic_load(&radio_state) != RADIO_STATE_ON) {
        completed(token, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
    struct serving_status serving;
    const uint8_t *body;
    size_t body_bytes;
    rc = nas_get(&serving, &body, &body_bytes);
    if (rc) {
        completed(token, modem_error(rc), NULL, 0);
        return;
    }
    if ((serving.registration != 1 && serving.registration != RIL_REG_ROAMING) || serving.ps != 1) {
        completed(token, RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW, NULL, 0);
        return;
    }
    rc = data_link_prepare(&packet_data.link);
    if (rc) {
        completed(token, RIL_E_NO_RESOURCES, NULL, 0);
        return;
    }
    int client = qmux_client(&transport, 1);
    if (client < 0) {
        int cleanup = data_cleanup();
        completed(token, cleanup ? RIL_E_NO_RESOURCES : modem_error(client), NULL, 0);
        return;
    }
    packet_data.client_owned = 1;
    uint8_t bind[] = {1, 2, 0, 4, 14}, family[] = {1, 1, 0, 4};
    int state = data_status_get();
    rc = state != 1 ? -EBUSY : request(1, 0x89, bind, sizeof(bind), &body, &body_bytes);
    if (!rc)
        rc = request(1, 0x4d, family, sizeof(family), &body, &body_bytes);
    uint8_t payload[512];
    int length = data_start_request(payload, sizeof(payload), &profile);
    int64_t now = monotonic_ms();
    if (!rc && (length < 0 || now < 0))
        rc = -EINVAL;
    if (rc) {
        int cleanup = data_cleanup();
        completed(token, cleanup ? RIL_E_NO_RESOURCES : modem_error(rc), NULL, 0);
        return;
    }
    packet_data.profile = profile;
    packet_data.token = token;
    packet_data.setup_pending = 1;
    packet_data.phase = DATA_STARTING;
    packet_data.deadline = now + 30000;
    atomic_store(&cancellable_data_token, token);
    rc = qmux_send_async(&transport, 1, client, 0x20, payload, length, data_async_response);
    if (rc) {
        (void)data_cleanup();
        data_complete(modem_error(rc));
    }
}
static void data_cancel(RIL_Token token) {
    if (token)
        atomic_compare_exchange_strong(&cancellable_data_token, &token, NULL);
}
static void data_shutdown(void) {
    if (packet_data.phase == DATA_IDLE && packet_data.link.fd < 0)
        return;
    int rc = data_cleanup();
    data_complete(RIL_E_CANCELLED);
    if (rc)
        __android_log_print(ANDROID_LOG_ERROR, "SupermanRIL", "Data shutdown retained=%d", rc);
}
static void data_request(int code, void *args, size_t bytes, RIL_Token token) {
    if (code == RIL_REQUEST_SETUP_DATA_CALL) {
        data_setup(args, bytes, token);
        return;
    }
    if (code == RIL_REQUEST_DATA_CALL_LIST) {
        if (args || bytes) {
            completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            return;
        }
        int refresh = data_refresh();
        if (refresh) {
            completed(token, modem_error(refresh), NULL, 0);
            return;
        }
        RIL_Data_Call_Response_v11 response;
        struct data_response_strings strings;
        int visible = data_visible();
        if (visible)
            data_response(&response, &strings);
        completed(token, RIL_E_SUCCESS, visible ? &response : NULL, visible ? sizeof(response) : 0);
        return;
    }
    if (!args || bytes != 2 * sizeof(char *)) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    const char *const *strings = args;
    if (!strings[0] || strcmp(strings[0], "1") || !strings[1] ||
        (strcmp(strings[1], "0") && strcmp(strings[1], "1"))) {
        completed(token, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    if (packet_data.phase == DATA_IDLE) {
        completed(token, RIL_E_INVALID_STATE, NULL, 0);
        return;
    }
    int rc = data_cleanup();
    data_complete(RIL_E_CANCELLED);
    completed(token, rc ? modem_error(rc) : RIL_E_SUCCESS, NULL, 0);
}
static void data_publish(void) {
    pthread_mutex_lock(&lock);
    if (!packet_data.notify) {
        pthread_mutex_unlock(&lock);
        return;
    }
    packet_data.notify = 0;
    RIL_Data_Call_Response_v11 response;
    struct data_response_strings strings;
    int visible = data_visible();
    if (visible)
        data_response(&response, &strings);
    /* ifname must outlive the unlock just like the stack response strings. */
    char name[IFNAMSIZ];
    if (visible) {
        memcpy(name, packet_data.link.name, sizeof(name));
        response.ifname = name;
    }
    pthread_mutex_unlock(&lock);
    environment->OnUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, visible ? &response : NULL,
                                       visible ? sizeof(response) : 0);
}

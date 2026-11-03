// SPDX-License-Identifier: Apache-2.0
#define RIL_SHLIB
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <telephony/ril.h>
#include <unistd.h>
extern void superman_ril_shutdown(void);
static int completions, last_error = -1, network_preference = -1;
static void complete(RIL_Token token, RIL_Errno err, void *response, size_t size) {
    int code = (int)(uintptr_t)token;
    completions++;
    last_error = err;
    printf("RIL request %d error=%d bytes=%zu\n", code, err, size);
    if (err)
        return;
    assert(response || !size);
    if (code == RIL_REQUEST_DATA_CALL_LIST) assert(size == 0);
    if (code == RIL_REQUEST_GET_CURRENT_CALLS) {
        assert(size % sizeof(RIL_Call *) == 0 && size / sizeof(RIL_Call *) <= 16);
        printf("Current calls=%zu\n", size / sizeof(RIL_Call *));
    }
    if (code == RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE) {
        assert(size == sizeof(int)); printf("Selection mode=%d\n", *(int *)response);
    }
    if (code == RIL_REQUEST_GET_SIM_STATUS) {
        assert(size == sizeof(RIL_CardStatus_v6));
        RIL_CardStatus_v6 *c = response;
        assert(c->num_applications >= 0 && c->num_applications <= RIL_CARD_MAX_APPS);
        if (c->num_applications)
            printf("SIM app state=%d perso=%d PIN1=%d\n", c->applications[0].app_state,
                   c->applications[0].perso_substate, c->applications[0].pin1);
        printf("Physical SIM2 card=%d apps=%d active_index=%d\n", c->card_state,
               c->num_applications, c->gsm_umts_subscription_app_index);
    }
    if (code == RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE) {
        assert(size == sizeof(int));
        network_preference = *(int *)response;
        printf("Network preference=%d\n", network_preference);
    }
    if (code == RIL_REQUEST_GET_RADIO_CAPABILITY) {
        assert(size == sizeof(RIL_RadioCapability));
        printf("Actual radio family=%d\n", ((RIL_RadioCapability *)response)->rat);
    }
    if (code == RIL_REQUEST_SIM_IO) {
        assert(size == sizeof(RIL_SIM_IO_Response));
        RIL_SIM_IO_Response *r = response;
        assert(r->sw1 >= 0 && r->sw1 <= 255 && r->sw2 >= 0 && r->sw2 <= 255);
        printf("SIM IO SW=%02x%02x response-bytes=%zu (contents private)\n", r->sw1, r->sw2,
               r->simResponse ? strlen(r->simResponse) / 2 : 0);
    }
    if (code == RIL_REQUEST_GET_IMSI) {
        assert(size >= 6 && size <= 16);
        printf("IMSI read: %zu digits (value private)\n", strlen(response));
    }
    if (code == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        assert(size == 15 * sizeof(char *));
        char **v = response;
        printf("Voice registration=%s radio-tech=%s\n", v[0] ? v[0] : "unknown",
               v[3] ? v[3] : "unknown");
    }
    if (code == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        assert(size == 6 * sizeof(char *));
        char **v = response;
        printf("Packet registration=%s radio-tech=%s\n", v[0] ? v[0] : "unknown",
               v[3] ? v[3] : "unknown");
    }
    if (code == RIL_REQUEST_OPERATOR)
        assert(size == 3 * sizeof(char *));
    if (code == RIL_REQUEST_SIGNAL_STRENGTH)
        assert(size == sizeof(RIL_SignalStrength_v10));
    fflush(stdout);
}
static void unsolicited(int code, const void *value, size_t size) {
    (void)value;
    printf("RIL unsolicited %d bytes=%zu\n", code, size);
}
static void timed(RIL_TimedCallback callback, void *value, const struct timeval *delay) {
    (void)callback;
    (void)value;
    (void)delay;
}
int main(int argc, char **argv) {
    int soak = argc == 2 && !strcmp(argv[1], "--soak");
    int network = argc == 2 && !strcmp(argv[1], "--network");
    int power = network || (argc == 2 && !strcmp(argv[1], "--power"));
    if (argc != 1 && !power && !soak)
        return 2;
    struct RIL_Env env = {complete, unsolicited, timed, NULL};
    const RIL_RadioFunctions *ril = RIL_Init(&env, 0, NULL);
    assert(ril && ril->version == RIL_VERSION);
    atexit(superman_ril_shutdown);
    for (unsigned i = 0; i < 45 && ril->onStateRequest() == RADIO_STATE_UNAVAILABLE; i++)
        sleep(1);
    if (ril->onStateRequest() == RADIO_STATE_UNAVAILABLE) {
        fprintf(stderr, "RIL transport did not initialize\n");
        return 1;
    }
    int initial = ril->onStateRequest();
    printf("Initial RIL state=%d\n", initial);
    const int requests[] = {RIL_REQUEST_DATA_CALL_LIST,RIL_REQUEST_GET_SMSC_ADDRESS, RIL_REQUEST_GET_CURRENT_CALLS, RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE, RIL_REQUEST_GET_RADIO_CAPABILITY,
                            RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
                            RIL_REQUEST_GET_SIM_STATUS,
                            RIL_REQUEST_BASEBAND_VERSION,
                            RIL_REQUEST_VOICE_REGISTRATION_STATE,
                            RIL_REQUEST_DATA_REGISTRATION_STATE,
                            RIL_REQUEST_OPERATOR,
                            RIL_REQUEST_SIGNAL_STRENGTH,
                            RIL_REQUEST_VOICE_RADIO_TECH};
    for (unsigned i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        int before = completions;
        ril->onRequest(requests[i], NULL, 0, (RIL_Token)(uintptr_t)requests[i]);
        assert(completions == before + 1);
    }
    if (network_preference >= 0 && network_preference <= 3) {
        ril->onRequest(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, &network_preference,
                       sizeof(network_preference),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
    }
    assert(!ril->supports(9999));
    ril->onRequest(9999, NULL, 0, (RIL_Token)(uintptr_t)9999);
    assert(last_error == RIL_E_REQUEST_NOT_SUPPORTED);
    if (power) {
        assert(initial == RADIO_STATE_OFF);
        int restore_preference = network_preference;
        if (network) {
            int desired = 0;
            ril->onRequest(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, &desired, sizeof(desired),
                           (RIL_Token)(uintptr_t)RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
            printf("WCDMA/GSM request result=%d\n", last_error);
        }
        int on = 1;
        ril->onRequest(RIL_REQUEST_RADIO_POWER, &on, sizeof(on),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_RADIO_POWER);
        printf("Power-on result=%d state=%d\n", last_error, ril->onStateRequest());
        if (network) {
            for (unsigned i = 0; i < 6; i++) {
                sleep(10);
                ril->onRequest(RIL_REQUEST_VOICE_REGISTRATION_STATE, NULL, 0,
                               (RIL_Token)(uintptr_t)RIL_REQUEST_VOICE_REGISTRATION_STATE);
                ril->onRequest(RIL_REQUEST_DATA_REGISTRATION_STATE, NULL, 0,
                               (RIL_Token)(uintptr_t)RIL_REQUEST_DATA_REGISTRATION_STATE);
                ril->onRequest(RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE, NULL, 0,
                               (RIL_Token)(uintptr_t)RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE);
            }
        } else
            sleep(12);
        ril->onRequest(RIL_REQUEST_GET_SIM_STATUS, NULL, 0,
                       (RIL_Token)(uintptr_t)RIL_REQUEST_GET_SIM_STATUS);
        ril->onRequest(RIL_REQUEST_DEVICE_IDENTITY, NULL, 0,
                       (RIL_Token)(uintptr_t)RIL_REQUEST_DEVICE_IDENTITY);
        char *imsi_args[] = {""};
        ril->onRequest(RIL_REQUEST_GET_IMSI, imsi_args, sizeof(imsi_args),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_GET_IMSI);
        RIL_SIM_IO_v6 io = {.command = 0xc0, .fileid = 0x2fe2, .path = "3F00", .p3 = 15};
        ril->onRequest(RIL_REQUEST_SIM_IO, &io, sizeof(io),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_SIM_IO);
        io.command = 0xb0;
        io.p3 = 10;
        ril->onRequest(RIL_REQUEST_SIM_IO, &io, sizeof(io),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_SIM_IO);
        if (network && restore_preference >= 0 && restore_preference <= 3) {
            ril->onRequest(RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, &restore_preference,
                           sizeof(restore_preference),
                           (RIL_Token)(uintptr_t)RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
            printf("Original network preference restored result=%d\n", last_error);
        }
        on = 0;
        ril->onRequest(RIL_REQUEST_RADIO_POWER, &on, sizeof(on),
                       (RIL_Token)(uintptr_t)RIL_REQUEST_RADIO_POWER);
        if (last_error != RIL_E_SUCCESS || ril->onStateRequest() != RADIO_STATE_OFF) {
            fprintf(stderr, "Radio-off readback failed\n");
            return 1;
        }
    }
    assert(ril->supports(RIL_REQUEST_SETUP_DATA_CALL));
    assert(ril->supports(RIL_REQUEST_DEACTIVATE_DATA_CALL));
    ril->onRequest(RIL_REQUEST_SETUP_DATA_CALL, NULL, 0, (RIL_Token)(uintptr_t)27);
    assert(last_error == RIL_E_INVALID_ARGUMENTS);
    ril->onRequest(RIL_REQUEST_DEACTIVATE_DATA_CALL, NULL, 0, (RIL_Token)(uintptr_t)41);
    assert(last_error == RIL_E_INVALID_ARGUMENTS);
    if (ril->supports(RIL_REQUEST_DIAL)) {
        ril->onRequest(RIL_REQUEST_DIAL, NULL, 0, (RIL_Token)(uintptr_t)RIL_REQUEST_DIAL);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
        RIL_Dial invalid = {.address = "1;AT"};
        ril->onRequest(RIL_REQUEST_DIAL, &invalid, sizeof(invalid), (RIL_Token)(uintptr_t)RIL_REQUEST_DIAL);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
    }
    if (ril->supports(RIL_REQUEST_SEND_SMS)) {
        ril->onRequest(RIL_REQUEST_SEND_SMS, NULL, 0, (RIL_Token)(uintptr_t)RIL_REQUEST_SEND_SMS);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
        char *bad_sms[] = {NULL, "not-a-PDU"};
        ril->onRequest(RIL_REQUEST_SEND_SMS, bad_sms, sizeof(bad_sms), (RIL_Token)(uintptr_t)RIL_REQUEST_SEND_SMS);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
        ril->onRequest(RIL_REQUEST_SMS_ACKNOWLEDGE, NULL, 0, (RIL_Token)(uintptr_t)RIL_REQUEST_SMS_ACKNOWLEDGE);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
        int invalid_memory=2;
        ril->onRequest(RIL_REQUEST_REPORT_SMS_MEMORY_STATUS,&invalid_memory,sizeof(invalid_memory),(RIL_Token)(uintptr_t)RIL_REQUEST_REPORT_SMS_MEMORY_STATUS);
        assert(last_error == RIL_E_INVALID_ARGUMENTS);
    }
    if (soak) for (unsigned i = 0; i < 15; i++) {
        printf("Soak %u radio-state=%d\n", i, ril->onStateRequest()); fflush(stdout); sleep(5);
    }
    superman_ril_shutdown();
    puts("Native RIL callback/transport test completed; clients and endpoint released");
    return 0;
}

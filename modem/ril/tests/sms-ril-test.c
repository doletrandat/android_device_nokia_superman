// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#define RIL_SHLIB
#include "../superman-ril.c"
#include <assert.h>
static uint8_t mock_body[256];
static int status_reports, primary_client, event_reports;
static int fail_message = -1, requests, raw_sends, acks, last_error, last_unsol, unsols,
           completions, reply_reference;
static int reenter_ack, inject_during_start;
static char reply_address[35];
static const uint8_t route_list[] = {6, 0, 0, 0,   255, 2, 0, 1,   255, 2, 0, 2,   0,
                                     1, 0, 3, 255, 2,   0, 4, 255, 2,   0, 5, 255, 2};
int __android_log_print(int p, const char *t, const char *f, ...) {
    (void)p;
    (void)t;
    (void)f;
    return 0;
}
int __system_property_get(const char *n, char *v) {
    (void)n;
    v[0] = 0;
    return 0;
}
int qmux_open(struct qmux *q) {
    (void)q;
    return -ENODEV;
}
void qmux_close(struct qmux *q) { (void)q; }
int qmux_poll(struct qmux *q) {
    (void)q;
    return 0;
}
int qmux_client(struct qmux *q, uint8_t service) {
    q->clients[service] = 1;
    return 1;
}
int qmux_send_async(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                    const void *data, size_t bytes, qmux_async_callback callback) {
    (void)q;
    (void)service;
    (void)client;
    (void)message;
    (void)data;
    (void)bytes;
    (void)callback;
    assert(!"SMS regression unexpectedly started data");
    return -ENOTSUP;
}
int qmux_release_client(struct qmux *q, uint8_t service) {
    (void)q;
    (void)service;
    assert(!"SMS regression unexpectedly released data client");
    return -ENOTSUP;
}
int qmux_request(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                 const void *data, size_t size, const uint8_t **body, size_t *bytes) {
    (void)q;
    assert(service == 5 && client == 1);
    requests++;
    *body = mock_body;
    *bytes = 0;
    const uint8_t *p = data;
    switch (message) {
    case 0x4c:
        assert(size == 4 && p[3] == 0);
        break;
    case 0x33:
        mock_body[0] = 1;
        qmux_write16(mock_body + 1, sizeof(route_list));
        memcpy(mock_body + 3, route_list, sizeof(route_list));
        memcpy(mock_body + 3 + sizeof(route_list), (uint8_t[]){0x10, 1, 0, status_reports}, 4);
        *bytes = sizeof(route_list) + 7;
        break;
    case 0x45:
        assert(size == 4 && p[0] == 1 && p[3] <= 1);
        primary_client = p[3];
        if (primary_client && inject_during_start) {
            uint8_t event[] = {0x11, 11, 0, 0, 99, 0, 0, 0, 6, 3, 0, 0, 0, 0};
            assert(q->indication);
            q->indication(5, 1, event, sizeof(event));
        }
        break;
    case 1:
        assert(size == 4 && p[0] == 0x10 && p[3] <= 1);
        event_reports = p[3];
        break;
    case 0x20:
        assert(size == 15 && p[3] == 6 && p[6] == 0 && p[7] == 1);
        raw_sends++;
        memcpy(mock_body, (uint8_t[]){1, 2, 0, 35, 0}, 5);
        *bytes = 5;
        break;
    case 0x34:
        assert(!size);
        memcpy(mock_body, (uint8_t[]){1, 7, 0, '1', '4', '5', 3, '1', '2', '3'}, 10);
        *bytes = 10;
        break;
    case 0x37:
        assert(size >= 9 && p[7] == 1);
        acks++;
        break;
    case 0x3b:
        assert(size == 4 && p[3] <= 1);
        break;
    default:
        assert(!"Unexpected modem operation");
    }
    if (fail_message == message) {
        fail_message = -1;
        return -ETIMEDOUT;
    }
    return 0;
}
static void done(RIL_Token token, RIL_Errno error, void *value, size_t size) {
    last_error = error;
    completions++;
    int code = (int)(uintptr_t)token;
    if (error)
        return;
    if (code == RIL_REQUEST_SEND_SMS || code == RIL_REQUEST_SEND_SMS_EXPECT_MORE) {
        assert(size == sizeof(RIL_SMS_Response));
        reply_reference = ((RIL_SMS_Response *)value)->messageRef;
    }
    if (code == RIL_REQUEST_GET_SMSC_ADDRESS) {
        assert(size < sizeof(reply_address));
        memcpy(reply_address, value, size);
    }
}
static void received(int code, const void *value, size_t size) {
    last_unsol = code;
    unsols++;
    if (code == RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM) {
        assert(size == sizeof(int) && *(const int *)value == 1);
        return;
    }
    assert(code == RIL_UNSOL_RESPONSE_NEW_SMS || code == RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT);
    assert(value && size == 6);
    if (reenter_ack) {
        int args[] = {1, 0};
        on_request(RIL_REQUEST_SMS_ACKNOWLEDGE, args, sizeof(args),
                   (void *)(uintptr_t)RIL_REQUEST_SMS_ACKNOWLEDGE);
        assert(last_error == RIL_E_SUCCESS);
    }
}
static void ask(int code, void *data, size_t size) {
    int before = completions;
    on_request(code, data, size, (void *)(uintptr_t)code);
    assert(completions == before + 1);
}
static void inject(unsigned id, int ack_required, int status) {
    uint8_t event[] = {0x11, 11, 0, !ack_required, id, 0, 0, 0, 6, 3, 0, 0, status ? 2 : 0, 0};
    messaging_indication(5, 1, event, sizeof(event));
}
int main(void) {
    struct RIL_Env env = {.OnRequestComplete = done, .OnUnsolicitedResponse = received};
    environment = &env;
    transport.fd = 1;
    transport.indication = radio_indication;
    atomic_store(&radio_state, RADIO_STATE_ON);
    assert(!messaging_start() && messaging_ready && primary_client && event_reports &&
           !status_reports);
    const int codes[] = {RIL_REQUEST_SEND_SMS, RIL_REQUEST_SEND_SMS_EXPECT_MORE,
                         RIL_REQUEST_GET_SMSC_ADDRESS, RIL_REQUEST_SMS_ACKNOWLEDGE,
                         RIL_REQUEST_REPORT_SMS_MEMORY_STATUS};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        assert(supports(codes[i]));
    int count = requests;
    ask(RIL_REQUEST_SEND_SMS, NULL, 0);
    assert(last_error == RIL_E_INVALID_ARGUMENTS && requests == count);
    char *bad[] = {NULL, "1;AT"};
    ask(RIL_REQUEST_SEND_SMS, bad, sizeof(bad));
    assert(last_error == RIL_E_INVALID_ARGUMENTS && requests == count);
    char *args[] = {NULL, "01000181F1000000"};
    ask(RIL_REQUEST_SEND_SMS, args, sizeof(args));
    assert(last_error == RIL_E_SUCCESS && raw_sends == 1 && reply_reference == 35);
    ask(RIL_REQUEST_SEND_SMS_EXPECT_MORE, args, sizeof(args));
    assert(last_error == RIL_E_SUCCESS && raw_sends == 2);
    ask(RIL_REQUEST_GET_SMSC_ADDRESS, NULL, 0);
    assert(last_error == RIL_E_SUCCESS && !strcmp(reply_address, "+123"));
    int ack[] = {1, 0};
    ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
    assert(last_error == RIL_E_INVALID_STATE && !acks);
    inject(1, 1, 0);
    inject(1, 1, 0);
    assert(incoming_sms.count == 1);
    messaging_publish();
    assert(last_unsol == RIL_UNSOL_RESPONSE_NEW_SMS && unsols == 1 && incoming_sms.delivered);
    ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
    assert(last_error == RIL_E_SUCCESS && acks == 1 && !incoming_sms.count);
    inject(2, 0, 0);
    messaging_publish();
    ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
    assert(last_error == RIL_E_SUCCESS && acks == 1 && !incoming_sms.count);
    inject(3, 1, 1);
    messaging_publish();
    assert(last_unsol == RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT);
    ack[0] = 0;
    ack[1] = 0xd3;
    ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
    assert(last_error == RIL_E_SUCCESS && acks == 2 && !incoming_sms.count);
    ack[0] = 1;
    ack[1] = 0;
    inject(4, 1, 0);
    messaging_publish();
    fail_message = 0x37;
    ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
    assert(last_error != RIL_E_SUCCESS && !incoming_sms.count);
    reenter_ack = 1;
    inject(5, 1, 0);
    messaging_publish();
    assert(!incoming_sms.count);
    reenter_ack = 0;
    for (unsigned i = 10; i < 19; i++)
        inject(i, 1, 0);
    assert(incoming_sms.count == 8);
    for (unsigned i = 0; i < 8; i++) {
        messaging_publish();
        ask(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack));
        assert(last_error == RIL_E_SUCCESS);
    }
    assert(!incoming_sms.count);
    uint8_t sim_event[] = {0x10, 5, 0, 0, 0, 0, 0, 0};
    messaging_indication(5, 1, sim_event, sizeof(sim_event));
    messaging_publish();
    assert(last_unsol == RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM && !incoming_sms.count);
    int memory = 0;
    ask(RIL_REQUEST_REPORT_SMS_MEMORY_STATUS, &memory, sizeof(memory));
    assert(last_error == RIL_E_SUCCESS);
    memory = 2;
    count = requests;
    ask(RIL_REQUEST_REPORT_SMS_MEMORY_STATUS, &memory, sizeof(memory));
    assert(last_error == RIL_E_INVALID_ARGUMENTS && requests == count);
    messaging_stop();
    assert(!messaging_ready && !primary_client && !event_reports && !status_reports &&
           !incoming_sms.count);
    count = requests;
    ask(RIL_REQUEST_SEND_SMS, args, sizeof(args));
    assert(last_error == RIL_E_INVALID_MODEM_STATE && requests == count);
    for (unsigned i = 0; i < 4; i++) {
        int failures[] = {0x4c, 0x45, 1, 0x33};
        fail_message = failures[i];
        assert(messaging_start() < 0);
        assert(!messaging_ready && !primary_client && !event_reports && !status_reports);
    }
    status_reports = 1;
    assert(!messaging_start());
    messaging_stop();
    assert(status_reports == 1);
    status_reports = 0;
    assert(!messaging_start());
    messaging_stop();
    assert(!status_reports);
    inject_during_start = 1;
    assert(!messaging_start() && incoming_sms.count == 1);
    reenter_ack = 1;
    messaging_publish();
    reenter_ack = 0;
    assert(!incoming_sms.count);
    messaging_stop();
    puts("PASS actual Radio SMS requests/unsolicited delivery/reentrant ACK/queue/failure "
         "cleanup/unchanged original routes (mock modem only)");
}

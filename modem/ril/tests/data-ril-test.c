// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#define RIL_SHLIB
#include "../superman-ril.c"
#include <assert.h>
static int registered = 1, roaming, packet_status = 1, starts, stops, releases, binds, allocations;
static int link_open, link_applies, link_restores, fail_prepare, fail_apply, fail_restore;
static int fail_release, fail_stop, fail_message = -1, fail_allocate, abort_late_success;
static int completions, last_error, setup_completions, last_setup_error, last_data_active, last_cid;
static int data_notices, sms_notices, sms_acks, ack_during_pending, finish_during_ack;
static int primary_client, event_reports, cancel_in_settings, mock_mode;
static uint8_t mock_body[256];
static const char *profile_args[] = {"5", "0", "internet.example", "user", "password", "3", "IP"};
static const uint32_t owned_handle = 0x12345678;
static void finish_start(int result);
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
    if (q->clients[service])
        return q->clients[service];
    if (service == 1) {
        if (fail_allocate)
            return -ENOMEM;
        allocations++;
    }
    q->clients[service] = 1;
    return 1;
}
int qmux_release_client(struct qmux *q, uint8_t service) {
    assert(service == 1 && packet_data.client_owned);
    releases++;
    if (fail_release)
        return -EIO;
    q->clients[service] = 0;
    if (q->async_service == 1)
        q->async_callback = NULL;
    return 0;
}
int qmux_send_async(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                    const void *data, size_t bytes, qmux_async_callback callback) {
    assert(service == 1 && client == 1 && message == 0x20 && callback == data_async_response &&
           packet_data.setup_pending && packet_data.phase == DATA_STARTING);
    assert(!packet_data.link.address_owned && !link_applies);
    size_t n;
    const uint8_t *v = qmux_tlv(data, bytes, 0x19, &n);
    assert(v && n == 1 && *v == 4);
    v = qmux_tlv(data, bytes, 0x33, &n);
    assert(v && n == 1 && !*v);
    assert(q->async_callback == NULL);
    q->async_service = 1;
    q->async_client = 1;
    q->async_transaction = 123;
    q->async_callback = callback;
    starts++;
    return 0;
}
static size_t add32(size_t pos, unsigned type, uint32_t value) {
    mock_body[pos] = type;
    qmux_write16(mock_body + pos + 1, 4);
    for (unsigned i = 0; i < 4; i++)
        mock_body[pos + 3 + i] = value >> (8 * i);
    return pos + 7;
}
int qmux_request(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                 const void *data, size_t size, const uint8_t **body, size_t *bytes) {
    assert(client == 1);
    *body = mock_body;
    *bytes = 0;
    const uint8_t *p = data;
    if (service == 3) {
        if (message == 0x45) {
            assert(size == 4 && p[3] == 0);
        } else {
            assert(message == 0x24);
            uint8_t nas[] = {1, 6, 0, registered ? 1 : 0, 1, registered ? 1 : 0, 0, 1, 5};
            memcpy(mock_body, nas, sizeof(nas));
            *bytes = sizeof(nas);
            if (roaming) {
                memcpy(mock_body + *bytes, (uint8_t[]){0x10, 1, 0, 0}, 4);
                *bytes += 4;
            }
        }
    } else if (service == 1) {
        switch (message) {
        case 0x22:
            memcpy(mock_body, (uint8_t[]){1, 1, 0, packet_status}, 4);
            *bytes = 4;
            break;
        case 0x89:
            assert(size == 5 && !memcmp(data, (uint8_t[]){1, 2, 0, 4, 14}, 5));
            binds++;
            break;
        case 0x4d:
            assert(size == 4 && !memcmp(data, (uint8_t[]){1, 1, 0, 4}, 4));
            break;
        case 0x2d:
            if (cancel_in_settings) { cancel_in_settings = 0; data_cancel(packet_data.token); }
            assert(packet_status == 2 || packet_status == 3);
            *bytes = add32(*bytes, 0x1e, 0x0a000002);
            *bytes = add32(*bytes, 0x20, 0x0a000001);
            *bytes = add32(*bytes, 0x21, 0xfffffffc);
            *bytes = add32(*bytes, 0x15, 0x08080808);
            *bytes = add32(*bytes, 0x29, 1428);
            break;
        case 0x21: {
            assert(size == 7);
            uint32_t handle = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16) |
                              ((uint32_t)p[6] << 24);
            assert(handle == owned_handle);
            stops++;
            if (fail_stop)
                return -EIO;
            packet_status = 1;
            break;
        }
        case 2:
            assert(size == 5 && qmux_read16(p + 3) == 123);
            if (q->async_callback)
                finish_start(abort_late_success ? 0 : -ECANCELED);
            break;
        default:
            assert(!"Unexpected WDS command or synchronous StartNetwork");
        }
    } else if (service == 2) {
        if (message == 0x2e) { assert(size == 4); mock_mode = p[3]; }
        else { assert(message == 0x2d); memcpy(mock_body,(uint8_t[]){1,1,0,mock_mode},4); *bytes=4; }
    } else {
        assert(service == 5);
        switch (message) {
        case 0x4c:
            assert(size == 4 && p[3] == 0);
            break;
        case 0x33: {
            const uint8_t routes[] = {1,   26,  0, 6, 0, 0,   0, 255,  2,   0, 1,
                                      255, 2,   0, 2, 0, 1,   0, 3,    255, 2, 0,
                                      4,   255, 2, 0, 5, 255, 2, 0x10, 1,   0, 0};
            memcpy(mock_body, routes, sizeof(routes));
            *bytes = sizeof(routes);
            break;
        }
        case 0x45:
            primary_client = p[3];
            break;
        case 1:
            event_reports = p[3];
            break;
        case 0x37:
            sms_acks++;
            assert(packet_data.phase == DATA_STARTING);
            if (finish_during_ack)
                finish_start(0);
            break;
        default:
            assert(!"Unexpected SMS operation");
        }
    }
    if (fail_message == message) {
        fail_message = -1;
        return -EIO;
    }
    return 0;
}
int data_link_prepare(struct data_link *link) {
    assert(link->fd == -1 && !link_open);
    if (fail_prepare)
        return -EBUSY;
    memset(link, 0, sizeof(*link));
    link->fd = 42;
    link->index = 7;
    link->initial_flags = 0x90;
    link->initial_mtu = 1500;
    strcpy(link->name, "wwan0");
    link_open = 1;
    return 0;
}
int data_link_apply(struct data_link *link, const struct data_lease *lease) {
    assert(link_open && link->fd == 42 && packet_data.handle == owned_handle && packet_status != 1);
    link_applies++;
    link->address = lease->address;
    link->mask = lease->mask;
    link->selected_mtu = lease->mtu;
    link->flags_owned = link->mtu_owned = link->address_owned = link->mask_set = 1;
    return fail_apply ? -EIO : 0;
}
int data_link_restore(struct data_link *link) {
    if (link->fd < 0)
        return 0;
    assert(link_open && link->fd == 42);
    link_restores++;
    if (fail_restore)
        return -EIO;
    link_open = 0;
    memset(link, 0, sizeof(*link));
    link->fd = -1;
    return 0;
}
static void done(RIL_Token token, RIL_Errno error, void *value, size_t size) {
    int code = (int)(uintptr_t)token;
    completions++;
    last_error = error;
    if (code == RIL_REQUEST_SETUP_DATA_CALL) {
        setup_completions++;
        last_setup_error = error;
    }
    if (error)
        return;
    if (code == RIL_REQUEST_SETUP_DATA_CALL || code == RIL_REQUEST_DATA_CALL_LIST) {
        if (!size) {
            assert(code == RIL_REQUEST_DATA_CALL_LIST && !value);
            last_data_active = 0;
            return;
        }
        assert(size == sizeof(RIL_Data_Call_Response_v11));
        RIL_Data_Call_Response_v11 *r = value;
        assert(!r->status && r->cid == 1 && !strcmp(r->type, "IP") && !strcmp(r->ifname, "wwan0") &&
               !strcmp(r->addresses, "10.0.0.2/30") && !strcmp(r->gateways, "10.0.0.1") &&
               !strcmp(r->dnses, "8.8.8.8") && r->mtu == 1428);
        last_cid = r->cid;
        last_data_active = r->active;
    }
}
static void received(int code, const void *value, size_t size) {
    if (code == RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED) { assert(!value && !size); return; }
    if (code == RIL_UNSOL_DATA_CALL_LIST_CHANGED) {
        data_notices++;
        if (!size)
            assert(!value);
        return;
    }
    assert(code == RIL_UNSOL_RESPONSE_NEW_SMS && value && size == 6);
    sms_notices++;
    if (ack_during_pending) {
        int ack[] = {1, 0};
        on_request(RIL_REQUEST_SMS_ACKNOWLEDGE, ack, sizeof(ack),
                   (void *)(uintptr_t)RIL_REQUEST_SMS_ACKNOWLEDGE);
        assert(last_error == RIL_E_SUCCESS);
    }
}
static void ask(int code, void *args, size_t bytes) {
    on_request(code, args, bytes, (void *)(uintptr_t)code);
}
static void finish_start(int result) {
    assert(transport.async_callback == data_async_response);
    qmux_async_callback callback = transport.async_callback;
    transport.async_callback = NULL;
    uint8_t body[] = {2, 4, 0, 0, 0, 0, 0, 1, 4, 0, 0x78, 0x56, 0x34, 0x12};
    packet_status = result ? 1 : 2;
    callback(result, body, sizeof(body));
}
static void start(void) {
    int before = setup_completions;
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
    assert(setup_completions == before && packet_data.phase == DATA_STARTING &&
           packet_data.setup_pending && link_open && !link_applies &&
           transport.async_callback == data_async_response);
}
static void active(void) {
    start();
    finish_start(0);
    data_process();
    assert(last_setup_error == RIL_E_SUCCESS && packet_data.phase == DATA_ACTIVE &&
           last_data_active == 2 && link_applies == 1 && !packet_data.setup_pending &&
           packet_data.configured);
}
static void stop(void) {
    const char *args[] = {"1", "0"};
    ask(RIL_REQUEST_DEACTIVATE_DATA_CALL, (void *)args, sizeof(args));
    assert(last_error == RIL_E_SUCCESS && packet_data.phase == DATA_IDLE && !link_open &&
           !transport.clients[1]);
}
static void reset(void) {
    assert(!link_open && !packet_data.client_owned && !packet_data.setup_pending &&
           !transport.clients[1] && !transport.async_callback);
    memset(&packet_data, 0, sizeof(packet_data));
    packet_data.link.fd = -1;
    atomic_store(&cancellable_data_token, NULL);
    atomic_store(&radio_state, RADIO_STATE_ON);
    mock_mode = cancel_in_settings = 0;
    registered = 1;
    roaming = 0;
    packet_status = 1;
    starts = stops = releases = binds = allocations = link_applies = link_restores = 0;
    fail_prepare = fail_apply = fail_restore = fail_release = fail_stop = fail_allocate =
        abort_late_success = 0;
    fail_message = -1;
    last_error = last_setup_error = -1;
    last_data_active = 0;
}
int main(void) {
    struct RIL_Env env = {.OnRequestComplete = done, .OnUnsolicitedResponse = received};
    environment = &env;
    transport.fd = 1;
    transport.indication = radio_indication;
    atomic_store(&radio_state, RADIO_STATE_ON);
    reset();
    ask(RIL_REQUEST_DATA_CALL_LIST, NULL, 0);
    assert(last_error == RIL_E_SUCCESS && !last_data_active && !allocations);
    ask(RIL_REQUEST_SETUP_DATA_CALL, NULL, 0);
    assert(last_error == RIL_E_INVALID_ARGUMENTS && !starts && !link_open);
    registered = 0;
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
    assert(last_error == RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW && !allocations && !starts &&
           !link_open);
    registered = 1;
    active();
    assert(starts == 1 && binds == 1 && last_cid == 1);
    ask(RIL_REQUEST_DATA_CALL_LIST, NULL, 0);
    assert(last_data_active == 2);
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
    assert(last_error == RIL_E_SUCCESS && starts == 1);
    const char *different[] = {"5", "0", "other.example", "u", "p", "0", "IP"};
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)different, sizeof(different));
    assert(last_error == RIL_E_NO_RESOURCES && starts == 1);
    stop();
    reset();
    roaming = 1;
    active();
    stop();
    reset();
    fail_prepare = 1;
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
    assert(last_error == RIL_E_NO_RESOURCES && !allocations && !link_open);
    reset();
    fail_allocate = 1;
    ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
    assert(last_error != RIL_E_SUCCESS && !link_open);
    for (unsigned i = 0; i < 2; i++) {
        reset();
        fail_message = i ? 0x4d : 0x89;
        ask(RIL_REQUEST_SETUP_DATA_CALL, (void *)profile_args, sizeof(profile_args));
        assert(last_error != RIL_E_SUCCESS && !starts && !link_open && !transport.clients[1]);
    }
    reset();
    start();
    finish_start(14);
    data_process();
    assert(last_setup_error != RIL_E_SUCCESS && !link_open && !link_applies &&
           !transport.clients[1]);
    reset();
    start();
    finish_start(0);
    fail_message = 0x2d;
    data_process();
    assert(last_setup_error != RIL_E_SUCCESS && stops == 1 && !link_open && !link_applies);
    reset();
    start();
    finish_start(0);
    fail_apply = 1;
    data_process();
    assert(last_setup_error == RIL_E_SYSTEM_ERR && stops == 1 && !link_open &&
           !packet_data.configured);
    reset();
    start();
    pthread_mutex_lock(&lock);
    cancelled((void *)(uintptr_t)RIL_REQUEST_SETUP_DATA_CALL);
    pthread_mutex_unlock(&lock);
    assert(packet_data.setup_pending);
    data_process();
    assert(last_setup_error == RIL_E_CANCELLED && !link_open && !link_applies);
    reset();
    start();
    abort_late_success = 1;
    cancelled((void *)(uintptr_t)RIL_REQUEST_SETUP_DATA_CALL);
    data_process();
    assert(last_setup_error == RIL_E_CANCELLED && stops == 1 && !link_open && !link_applies);
    reset();
    start();
    packet_data.deadline = 0;
    data_process();
    assert(last_setup_error == RIL_E_NETWORK_ERR && !link_open && !transport.clients[1]);
    reset();
    active();
    fail_restore = 1;
    const char *deactivate[] = {"1", "0"};
    ask(RIL_REQUEST_DEACTIVATE_DATA_CALL, (void *)deactivate, sizeof(deactivate));
    assert(last_error != RIL_E_SUCCESS && packet_data.phase == DATA_RETAINED && link_open &&
           !transport.clients[1]);
    int before = allocations;
    ask(RIL_REQUEST_DATA_CALL_LIST, NULL, 0);
    assert(last_error == RIL_E_SUCCESS && !last_data_active && allocations == before);
    fail_restore = 0;
    stop();
    reset();
    active();
    fail_release = 1;
    ask(RIL_REQUEST_DEACTIVATE_DATA_CALL, (void *)deactivate, sizeof(deactivate));
    assert(last_error != RIL_E_SUCCESS && packet_data.phase == DATA_RETAINED &&
           packet_data.client_owned && link_open);
    fail_release = 0;
    stop();
    reset();
    active();
    fail_stop = 1;
    fail_message = 0x22;
    ask(RIL_REQUEST_DEACTIVATE_DATA_CALL, (void *)deactivate, sizeof(deactivate));
    assert(last_error != RIL_E_SUCCESS && packet_data.phase == DATA_RETAINED &&
           packet_data.handle == owned_handle && link_open);
    fail_stop = 0;
    stop();
    reset();
    assert(!messaging_start() && messaging_ready && primary_client && event_reports);
    start();
    ack_during_pending = finish_during_ack = 1;
    uint8_t sms[] = {0x11, 11, 0, 0, 3, 0, 0, 0, 6, 3, 0, 0, 0, 0};
    transport.indication(5, 1, sms, sizeof(sms));
    messaging_publish();
    assert(sms_notices == 1 && sms_acks == 1 && packet_data.response_ready &&
           packet_data.setup_pending);
    data_process();
    assert(last_setup_error == RIL_E_SUCCESS && last_data_active == 2);
    messaging_stop();
    assert(transport.indication == radio_indication);
    packet_status = 1;
    uint8_t lost[] = {1, 2, 0, 1, 0};
    transport.indication(1, 0x22, lost, sizeof(lost));
    data_process();
    data_publish();
    assert(packet_data.phase == DATA_IDLE && !link_open && data_notices > 0);
    reset();
    active();
    packet_status = 2;
    uint8_t reconfig[] = {1, 2, 0, 2, 1};
    transport.indication(1, 0x22, reconfig, sizeof(reconfig));
    data_process();
    assert(packet_data.phase == DATA_IDLE && !link_open && stops == 1);
    reset(); start(); finish_start(0); cancel_in_settings=1; data_process();
    assert(last_setup_error == RIL_E_CANCELLED && packet_data.phase == DATA_IDLE && !link_open && stops==1);
    reset(); active(); int off=0;
    ask(RIL_REQUEST_RADIO_POWER,&off,sizeof(off));
    assert(last_error==RIL_E_SUCCESS && state_get()==RADIO_STATE_OFF && !link_open && stops==1);
    reset(); start(); ask(RIL_REQUEST_RADIO_POWER,&off,sizeof(off));
    assert(last_error==RIL_E_SUCCESS && last_setup_error==RIL_E_CANCELLED && !link_open);
    puts("PASS actual data Radio controller: async setup, truthful results, roaming/unregistered "
         "guard, same-profile reuse, cancellation/deadline, late allocation cleanup, owned cleanup "
         "retries and SMS ACK during pending data");
}

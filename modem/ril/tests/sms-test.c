// SPDX-License-Identifier: Apache-2.0
#include "../sms.h"
#include "../../qmux-codec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t out[512];
    memset(out, 0xa5, sizeof(out));
    int n = sms_send_request(out, sizeof(out), NULL, "01000181F1000000");
    assert(n == 15 && out[0] == 1 && qmux_read16(out + 1) == 12 && out[3] == 6 &&
           qmux_read16(out + 4) == 9 && out[6] == 0 && out[7] == 1);
    assert(sms_send_request(out, 14, NULL, "01000181F1000000") == -ENOSPC);
    assert(sms_send_request(out, sizeof(out), "029121", "01000181F1000000") == 17);
    assert(sms_send_request(out, sizeof(out), "039121", "01000181F1000000") == -EINVAL);
    assert(sms_send_request(out, sizeof(out), NULL, NULL) == -EINVAL);
    assert(sms_send_request(out, sizeof(out), NULL, "") == -EINVAL);
    assert(sms_send_request(out, sizeof(out), NULL, "01000181F100000G") == -EINVAL);
    assert(sms_send_request(out, sizeof(out), NULL, "00000181F1000000") == -EINVAL);
    assert(sms_send_request(out, sizeof(out), NULL, "01000181F100000") == -EINVAL);
    char huge[355];
    memset(huge, '0', 354);
    huge[354] = 0;
    huge[1] = '1';
    assert(sms_send_request(out, sizeof(out), NULL, huge) == -EINVAL);
    int reference = -1;
    uint8_t sent[] = {1, 2, 0, 0x34, 0x12};
    assert(!sms_send_response(sent, sizeof(sent), &reference) && reference == 0x1234);
    assert(sms_send_response(sent, sizeof(sent) - 1, &reference) == -EBADMSG);
    char address[35];
    uint8_t smsc[] = {1, 7, 0, '1', '4', '5', 3, '1', '2', '3'};
    assert(!sms_smsc_response(smsc, sizeof(smsc), address, sizeof(address)) &&
           !strcmp(address, "+123"));
    assert(sms_smsc_response(smsc, sizeof(smsc), address, 4) == -ENOSPC);
    smsc[8] = ';';
    assert(sms_smsc_response(smsc, sizeof(smsc), address, sizeof(address)) == -EBADMSG);
    uint8_t routes[] = {1, 26, 0,   6, 0, 0, 0,   255, 2, 0, 1,   255, 2,    0, 2, 0, 1,
                        0, 3,  255, 2, 0, 4, 255, 2,   0, 5, 255, 2,   0x10, 1, 0, 0};
    assert(!sms_routes_transfer(routes, sizeof(routes)));
    routes[7] = 0;
    assert(sms_routes_transfer(routes, sizeof(routes)) == -ENOTSUP);
    routes[7] = 255;
    uint8_t event[] = {0x11, 11, 0, 0, 0x78, 0x56, 0x34, 0x12, 6, 3, 0, 0, 0, 0};
    struct sms_event e;
    assert(sms_event_decode(event, sizeof(event), &e) == 1 && e.ack_required &&
           e.transaction == 0x12345678 && e.sim_index == -1 && !e.status_report &&
           !strcmp(e.pdu, "000000"));
    n = sms_ack_request(out, sizeof(out), &e, 1, 0);
    assert(n == 9 && out[3] == 0x78 && out[6] == 0x12 && out[7] == 1 && out[8] == 1);
    n = sms_ack_request(out, sizeof(out), &e, 0, 0xd3);
    assert(n == 14 && out[12] == 22 && out[13] == 0xd3);
    e.ims = 1;
    assert(sms_ack_request(out, sizeof(out), &e, 0, 255) == 18);
    assert(sms_ack_request(out, 17, &e, 0, 255) == -ENOSPC);
    event[12] = 2;
    assert(sms_event_decode(event, sizeof(event), &e) == 1 && e.status_report);
    event[12] = 1;
    assert(sms_event_decode(event, sizeof(event), &e) == -EBADMSG);
    event[12] = 0;
    event[3] = 2;
    assert(sms_event_decode(event, sizeof(event), &e) == -EBADMSG);
    event[3] = 0;
    uint8_t stored[] = {0x10, 5, 0, 0, 0, 0, 0, 0};
    assert(sms_event_decode(stored, sizeof(stored), &e) == 1 && e.sim_index == 1);
    stored[4] = 255;
    stored[5] = 255;
    stored[6] = 255;
    stored[7] = 255;
    assert(sms_event_decode(stored, sizeof(stored), &e) == -EBADMSG);
    uint8_t duplicate[2 * sizeof(event)];
    memcpy(duplicate, event, sizeof(event));
    memcpy(duplicate + sizeof(event), event, sizeof(event));
    assert(sms_event_decode(duplicate, sizeof(duplicate), &e) == -EBADMSG);
    struct sms_queue q = {0};
    assert(sms_event_decode(event, sizeof(event), &e) == 1);
    for (unsigned i = 0; i < SUPERMAN_SMS_QUEUE; i++) {
        e.transaction = i;
        assert(sms_queue_push(&q, &e) == 1);
        assert(sms_queue_push(&q, &e) == 0);
    }
    e.transaction = 99;
    assert(sms_queue_push(&q, &e) == -ENOSPC);
    q.delivered = 1;
    sms_queue_pop(&q);
    assert(q.count == 7 && !q.delivered);
    assert(sms_queue_push(&q, &e) == 1);
    while (q.count)
        sms_queue_pop(&q);
    assert(!q.count);
    unsigned seed = 1;
    for (unsigned round = 0; round < 65536; round++) {
        uint8_t data[512];
        size_t size = round % sizeof(data);
        for (size_t i = 0; i < size; i++) {
            seed = seed * 1664525u + 1013904223u;
            data[i] = seed >> 24;
        }
        (void)sms_event_decode(data, size, &e);
        (void)sms_routes_transfer(data, size);
        (void)sms_send_response(data, size, &reference);
        (void)sms_smsc_response(data, size, address, sizeof(address));
    }
    puts("PASS SMS bounds/builders/routes/queue/ACK plus65536 malformed payloads");
}

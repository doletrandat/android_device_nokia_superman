// SPDX-License-Identifier: Apache-2.0
// WMS layouts: libqmi qmi-service-wms.json and Qualcomm Gobi API 2013-07-31.
#include "sms.h"
#include "../qmux-codec.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int unhex(const char *text, uint8_t *out, size_t max) {
    if (!text)
        return -EINVAL;
    size_t n = strnlen(text, 2 * max + 1);
    if (!n || n > 2 * max || (n & 1))
        return -EINVAL;
    for (size_t i = 0; i < n; i += 2) {
        int a = nibble(text[i]), b = nibble(text[i + 1]);
        if (a < 0 || b < 0)
            return -EINVAL;
        out[i / 2] = (a << 4) | b;
    }
    return n / 2;
}
int sms_send_request(uint8_t *out, size_t capacity, const char *smsc, const char *tpdu) {
    uint8_t pdu[SUPERMAN_SMS_MAX_PDU];
    int address = 1;
    pdu[0] = 0;
    if (smsc && *smsc) {
        address = unhex(smsc, pdu, 12);
        if (address < 1 || pdu[0] != address - 1)
            return -EINVAL;
    }
    int data = unhex(tpdu, pdu + address, 176);
    if (data < 7 || (pdu[address] & 3) != 1)
        return -EINVAL;
    size_t total = address + data;
    if (!out || capacity < total + 6)
        return -ENOSPC;
    out[0] = 1;
    qmux_write16(out + 1, total + 3);
    out[3] = 6;
    qmux_write16(out + 4, total);
    memcpy(out + 6, pdu, total);
    return total + 6;
}
int sms_send_response(const uint8_t *body, size_t bytes, int *reference) {
    if (!qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &n);
    if (!v || n != 2 || !reference)
        return -EBADMSG;
    *reference = qmux_read16(v);
    return 0;
}
int sms_smsc_response(const uint8_t *body, size_t bytes, char *out, size_t capacity) {
    if (!qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &n);
    if (!v || n < 4 || v[3] != n - 4 || v[3] > 32 || !out)
        return -EBADMSG;
    for (unsigned i = 0; i < 3; i++)
        if (v[i] < '0' || v[i] > '9')
            return -EBADMSG;
    unsigned type = 100u * (v[0] - '0') + 10u * (v[1] - '0') + v[2] - '0';
    if (type > 255)
        return -EBADMSG;
    int prefix = type == 145 && v[3] && v[4] != '+';
    if (capacity < (size_t)v[3] + prefix + 1)
        return -ENOSPC;
    for (unsigned i = 0; i < v[3]; i++)
        if (!((v[4 + i] >= '0' && v[4 + i] <= '9') || (i == 0 && v[4 + i] == '+')))
            return -EBADMSG;
    if (prefix)
        out[0] = '+';
    memcpy(out + prefix, v + 4, v[3]);
    out[prefix + v[3]] = 0;
    return 0;
}
int sms_routes_transfer(const uint8_t *body, size_t bytes) {
    if (!qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &n);
    if (!v || n < 2 || qmux_read16(v) > 32 || n != 2 + 4u * qmux_read16(v))
        return -EBADMSG;
    unsigned found = 0;
    for (unsigned i = 0; i < qmux_read16(v); i++) {
        const uint8_t *r = v + 2 + 4 * i;
        if (r[0] || r[1] > 5 || r[3] > 3)
            return -EBADMSG;
        if (r[1] == 2 || r[1] == 5)
            continue;
        if ((found & (1u << r[1])) || r[2] != 255 || r[3] != 2)
            return -ENOTSUP;
        found |= 1u << r[1];
    }
    return found == 0x1b ? 0 : -ENOTSUP;
}
int sms_event_decode(const uint8_t *body, size_t bytes, struct sms_event *event) {
    if (!event)
        return -EINVAL;
    if (!qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    struct sms_event e = {.sim_index = -1};
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 0x11, &n);
    if (v) {
        if (n < 8 || v[0] > 1 || v[5] != 6 || qmux_read16(v + 6) != n - 8 ||
            n - 8 > SUPERMAN_SMS_MAX_PDU)
            return -EBADMSG;
        const uint8_t *p = v + 8;
        size_t size = n - 8;
        if (size < 2 || p[0] > 12 || size <= (size_t)p[0] + 1)
            return -EBADMSG;
        unsigned mti = p[1 + p[0]] & 3;
        if (mti != 0 && mti != 2)
            return -EBADMSG;
        e.status_report = mti == 2;
        e.ack_required = !v[0];
        e.transaction = (uint32_t)v[1] | ((uint32_t)v[2] << 8) | ((uint32_t)v[3] << 16) |
                        ((uint32_t)v[4] << 24);
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < size; i++) {
            e.pdu[2 * i] = hex[p[i] >> 4];
            e.pdu[2 * i + 1] = hex[p[i] & 15];
        }
        v = qmux_tlv(body, bytes, 0x16, &n);
        if (v) {
            if (n != 1 || *v > 1)
                return -EBADMSG;
            e.ims = *v;
        }
    } else {
        v = qmux_tlv(body, bytes, 0x10, &n);
        if (!v)
            return 0;
        if (n != 5)
            return -EBADMSG;
        if (v[0] != 0)
            return -ENOTSUP;
        uint32_t index = (uint32_t)v[1] | ((uint32_t)v[2] << 8) | ((uint32_t)v[3] << 16) |
                         ((uint32_t)v[4] << 24);
        if (index >= INT_MAX)
            return -EBADMSG;
        e.sim_index = (int)index + 1;
    }
    *event = e;
    return 1;
}
int sms_queue_push(struct sms_queue *q, const struct sms_event *event) {
    if (!q || !event || q->count > SUPERMAN_SMS_QUEUE || q->head >= SUPERMAN_SMS_QUEUE)
        return -EINVAL;
    for (unsigned i = 0; i < q->count; i++) {
        const struct sms_event *e = &q->events[(q->head + i) % SUPERMAN_SMS_QUEUE];
        if (e->transaction == event->transaction && e->ack_required == event->ack_required &&
            e->ims == event->ims && e->status_report == event->status_report &&
            e->sim_index == event->sim_index && !memcmp(e->pdu, event->pdu, sizeof(e->pdu)))
            return 0;
    }
    if (q->count == SUPERMAN_SMS_QUEUE)
        return -ENOSPC;
    q->events[(q->head + q->count) % SUPERMAN_SMS_QUEUE] = *event;
    q->count++;
    return 1;
}
void sms_queue_pop(struct sms_queue *q) {
    if (!q || !q->count)
        return;
    memset(&q->events[q->head], 0, sizeof(q->events[q->head]));
    q->head = (q->head + 1) % SUPERMAN_SMS_QUEUE;
    q->count--;
    q->delivered = 0;
}
int sms_ack_request(uint8_t *out, size_t capacity, const struct sms_event *event, int success,
                    int cause) {
    if (!event || (success != 0 && success != 1) || cause < 0 || cause > 255)
        return -EINVAL;
    size_t n = 9 + (success ? 0 : 5) + (event->ims ? 4 : 0);
    if (!out || capacity < n)
        return -ENOSPC;
    out[0] = 1;
    qmux_write16(out + 1, 6);
    for (unsigned i = 0; i < 4; i++)
        out[3 + i] = event->transaction >> (8 * i);
    out[7] = 1;
    out[8] = success;
    n = 9;
    if (!success) {
        out[n++] = 0x11;
        out[n++] = 2;
        out[n++] = 0;
        out[n++] = cause == 0xd3 ? 22 : 111;
        out[n++] = cause;
    }
    if (event->ims) {
        out[n++] = 0x12;
        out[n++] = 1;
        out[n++] = 0;
        out[n++] = 1;
    }
    return n;
}

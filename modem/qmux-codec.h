// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_QMUX_CODEC_H
#define SUPERMAN_QMUX_CODEC_H
#include <stddef.h>
#include <stdint.h>
static inline uint16_t qmux_read16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline void qmux_write16(uint8_t *p, uint16_t x) {
    p[0] = x;
    p[1] = x >> 8;
}
/* QMI TLVs are singletons; reject malformed or ambiguous indication payloads. */
static inline int qmux_tlvs_valid(const uint8_t *p, size_t n) {
    uint8_t seen[32] = {0};
    while (n) {
        if (n < 3)
            return 0;
        unsigned type = p[0];
        size_t len = qmux_read16(p + 1);
        if (len > n - 3 || (seen[type / 8] & (1u << (type % 8))))
            return 0;
        seen[type / 8] |= 1u << (type % 8);
        p += 3 + len;
        n -= 3 + len;
    }
    return 1;
}
/* One service indication, zero other valid frame, negative malformed frame. */
static inline int qmux_indication(const uint8_t *p, size_t n, uint8_t *svc, uint8_t *cid,
                                  uint16_t *msg, const uint8_t **body, size_t *bytes) {
    if (n < 6 || p[0] != 1 || qmux_read16(p + 1) != n - 1 || p[3] != 0x80)
        return -1;
    size_t h = p[4] ? 13 : 12;
    if (n < h || qmux_read16(p + (p[4] ? 11 : 10)) != n - h)
        return -1;
    if (!p[4] || p[6] != 4)
        return 0;
    if (!qmux_tlvs_valid(p + h, n - h))
        return -1;
    *svc = p[4];
    *cid = p[5];
    *msg = qmux_read16(p + 9);
    *body = p + h;
    *bytes = n - h;
    return 1;
}
/* Return -2 for unrelated responses/indications, -1 for malformed frames. */
static inline int qmux_response(const uint8_t *p, size_t n, uint8_t svc, uint8_t cid, uint16_t txn,
                                uint16_t msg, const uint8_t **tlvs, size_t *bytes) {
    if (n < 6 || p[0] != 1 || qmux_read16(p + 1) != n - 1 || p[3] != 0x80)
        return -1;
    // CTL indications have a 12-byte header, even while awaiting a 13-byte service response.
    size_t h = p[4] ? 13 : 12;
    if (n < h || qmux_read16(p + (p[4] ? 11 : 10)) != n - h)
        return -1;
    if (p[4] != svc || p[5] != cid || p[6] != (svc ? 2 : 1) ||
        (svc ? qmux_read16(p + 7) : p[7]) != txn || qmux_read16(p + (svc ? 9 : 8)) != msg)
        return -2;
    if (qmux_read16(p + (svc ? 11 : 10)) != n - h)
        return -1;
    size_t off = h;
    int result = -1, seen = 0;
    while (off < n) {
        if (n - off < 3)
            return -1;
        unsigned type = p[off];
        size_t len = qmux_read16(p + off + 1);
        off += 3;
        if (len > n - off)
            return -1;
        if (type == 2) {
            if (len != 4 || seen++)
                return -1;
            result = qmux_read16(p + off) ? 1 : 0;
        }
        off += len;
    }
    if (!seen)
        return -1;
    *tlvs = p + h;
    *bytes = n - h;
    return result;
}
static inline const uint8_t *qmux_tlv(const uint8_t *p, size_t n, unsigned type, size_t *len) {
    const uint8_t *found = NULL;
    while (n) {
        if (n < 3)
            return NULL;
        size_t l = qmux_read16(p + 1);
        if (l > n - 3)
            return NULL;
        if (p[0] == type) {
            if (found)
                return NULL;
            found = p + 3;
            *len = l;
        }
        p += 3 + l;
        n -= 3 + l;
    }
    return found;
}
#endif

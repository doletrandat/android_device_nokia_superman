// SPDX-License-Identifier: Apache-2.0
#include "sim-io.h"
#include "../qmux-codec.h"
#include "model.h"
#include <errno.h>
#include <string.h>
int uim_file_request(uint8_t *out, size_t capacity, unsigned subscription, const uint8_t *aid,
                     size_t aid_bytes, int command, int file, const char *path, int p1, int p2,
                     int p3) {
    if (subscription > 1 || aid_bytes > 32 || (!aid && aid_bytes) || file < 0 || file > 65535 ||
        p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255 || p3 < 0 || p3 > 255)
        return -EINVAL;
    if (command != 0xb0 && command != 0xb2 && command != 0xc0)
        return -ENOTSUP;
    if (command == 0xb2 && (p1 == 0 || (p2 & 7) != 4))
        return -ENOTSUP;
    uint8_t hierarchy[12];
    int hierarchy_bytes = bytes_from_hex(path, hierarchy, sizeof(hierarchy));
    if (hierarchy_bytes < 2 || hierarchy_bytes % 2)
        return -EINVAL;
    size_t needed = 3 + 2 + aid_bytes + 3 + 3 + hierarchy_bytes + (command == 0xc0 ? 0 : 7);
    if (needed > capacity)
        return -ENOSPC;
    size_t off = 0;
    out[off++] = 1;
    qmux_write16(out + off, 2 + aid_bytes);
    off += 2;
    out[off++] = subscription * 2;
    out[off++] = aid_bytes;
    if (aid_bytes) {
        memcpy(out + off, aid, aid_bytes);
        off += aid_bytes;
    }
    out[off++] = 2;
    qmux_write16(out + off, 3 + hierarchy_bytes);
    off += 2;
    qmux_write16(out + off, file);
    off += 2;
    out[off++] = hierarchy_bytes;
    for (int i = 0; i < hierarchy_bytes; i += 2) {
        out[off++] = hierarchy[i + 1];
        out[off++] = hierarchy[i];
    }
    if (command != 0xc0) {
        out[off++] = 3;
        qmux_write16(out + off, 4);
        off += 2;
        qmux_write16(out + off, command == 0xb0 ? (p1 << 8) | p2 : p1);
        off += 2;
        qmux_write16(out + off, p3 ? p3 : 256);
        off += 2;
    }
    return off;
}
int uim_file_response(const uint8_t *body, size_t bytes, int command, int file,
                      const uint8_t **contents, size_t *length, int *sw1, int *sw2) {
    size_t n;
    const uint8_t *card = qmux_tlv(body, bytes, 0x10, &n);
    if (card && n != 2)
        return -EINVAL;
    *sw1 = card ? card[0] : 0x90;
    *sw2 = card ? card[1] : 0;
    const uint8_t *v = qmux_tlv(body, bytes, 0x11, &n);
    if (!v) {
        *contents = NULL;
        *length = 0;
        return card ? 0 : -EINVAL;
    }
    size_t prefix = command == 0xc0 ? 26 : 2;
    if (n < prefix)
        return -EINVAL;
    if (command == 0xc0 && qmux_read16(v + 2) != file)
        return -EINVAL;
    size_t size = qmux_read16(v + prefix - 2);
    if (size != n - prefix || size > 512)
        return -EINVAL;
    *contents = v + prefix;
    *length = size;
    return 0;
}
int imsi_decode(const uint8_t *data, size_t bytes, char out[16]) {
    if (!data || bytes < 2 || data[0] < 1 || data[0] > 8 || (size_t)data[0] + 1 > bytes)
        return -EINVAL;
    unsigned first = data[1] >> 4;
    if (first > 9)
        return -EINVAL;
    size_t n = 0;
    out[n++] = '0' + first;
    for (unsigned i = 2; i <= data[0]; i++)
        for (unsigned shift = 0; shift < 8; shift += 4) {
            unsigned digit = (data[i] >> shift) & 15;
            if (digit == 15) {
                if (shift != 4 || i != data[0])
                    return -EINVAL;
                continue;
            }
            if (digit > 9 || n >= 15)
                return -EINVAL;
            out[n++] = '0' + digit;
        }
    if (n < 5)
        return -EINVAL;
    out[n] = 0;
    return n;
}

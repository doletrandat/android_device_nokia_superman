// SPDX-License-Identifier: Apache-2.0
#include "../data-call.h"
#include "../../qmux-codec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static size_t add32(uint8_t *body, size_t pos, unsigned type, uint32_t value) {
    body[pos] = type;
    qmux_write16(body + pos + 1, 4);
    for (unsigned i = 0; i < 4; i++)
        body[pos + 3 + i] = value >> (8 * i);
    return pos + 7;
}
int main(void) {
    const char *args[] = {"5", "0", "internet.example", "u", "p", "3", "IPV4V6"};
    struct data_profile p;
    assert(!data_profile_parse(args, sizeof(args), &p) && !strcmp(p.apn, "internet.example") &&
           !strcmp(p.user, "u") && p.authentication == 3);
    uint8_t out[512];
    int n = data_start_request(out, sizeof(out), &p);
    assert(n > 0 && qmux_tlvs_valid(out, n));
    size_t len;
    const uint8_t *v = qmux_tlv(out, n, 0x19, &len);
    assert(v && len == 1 && *v == 4);
    v = qmux_tlv(out, n, 0x33, &len);
    assert(v && len == 1 && !*v);
    v = qmux_tlv(out, n, 0x17, &len);
    assert(v && len == 1 && *v == 'u');
    assert(!qmux_tlv(out, n, 0x31, &len));
    for (int cap = 0; cap < n; cap++)
        assert(data_start_request(out, cap, &p) == -ENOSPC);
    assert(data_profile_parse(NULL, sizeof(args), &p) == -EINVAL);
    assert(data_profile_parse(args, 6 * sizeof(char *), &p) == -EINVAL);
    const char *bad_apns[] = {"internet;AT", "a..b", "-a.b", "a-.b", "a.", "a b"};
    for (unsigned i = 0; i < sizeof(bad_apns) / sizeof(bad_apns[0]); i++) {
        args[2] = bad_apns[i];
        assert(data_profile_parse(args, sizeof(args), &p) == -EINVAL);
    }
    char too_long[102];
    memset(too_long, 'a', 101);
    too_long[101] = 0;
    args[2] = too_long;
    assert(data_profile_parse(args, sizeof(args), &p) == -EINVAL);
    args[2] = "";
    args[3] = NULL;
    args[4] = NULL;
    args[5] = "0";
    assert(!data_profile_parse(args, sizeof(args), &p) && !p.apn[0] && !p.user[0] &&
           !p.password[0]);
    args[5] = "4";
    assert(data_profile_parse(args, sizeof(args), &p) == -EINVAL);
    args[5] = "0";
    args[6] = "IPV6";
    assert(data_profile_parse(args, sizeof(args), &p) == -ENOTSUP);
    args[6] = "IP";
    args[0] = "1000000";
    assert(data_profile_parse(args, sizeof(args), &p) == -EINVAL);
    args[0] = "8";
    assert(data_profile_parse(args, sizeof(args), &p) == -ENOTSUP);
    uint8_t handle[] = {1, 4, 0, 0x78, 0x56, 0x34, 0x12};
    uint32_t h = 0;
    assert(!data_handle_decode(handle, sizeof(handle), &h) && h == 0x12345678);
    memset(handle + 3, 255, 4);
    assert(data_handle_decode(handle, sizeof(handle), &h) == -EBADMSG);
    memset(handle + 3, 0, 4);
    assert(data_handle_decode(handle, sizeof(handle), &h) == -EBADMSG);
    uint8_t status[] = {1, 1, 0, 1};
    for (unsigned i = 1; i <= 4; i++) {
        status[3] = i;
        assert(data_packet_status(status, sizeof(status)) == (int)i);
    }
    status[3] = 5;
    assert(data_packet_status(status, sizeof(status)) == -EBADMSG);
    uint8_t body[128];
    size_t used = 0;
    used = add32(body, used, 0x1e, 0x0a141e28);
    used = add32(body, used, 0x20, 0x0a141e29);
    used = add32(body, used, 0x21, 0xfffffffc);
    used = add32(body, used, 0x15, 0x08080808);
    used = add32(body, used, 0x16, 0x01010101);
    used = add32(body, used, 0x29, 1428);
    struct data_lease l;
    assert(!data_settings_decode(body, used, &l) && l.address == 0x0a141e28 &&
           l.gateway == 0x0a141e29 && l.prefix == 30 && l.mtu == 1428);
    char text[16];
    data_ipv4_string(l.address, text);
    assert(!strcmp(text, "10.20.30.40"));
    for (unsigned prefix = 1; prefix <= 32; prefix++) {
        uint32_t mask = ~0u << (32 - prefix);
        for (unsigned i = 0; i < 4; i++)
            body[17 + i] = mask >> (8 * i);
        assert(!data_settings_decode(body, used, &l) && l.prefix == prefix);
    }
    for (unsigned i = 0; i < 4; i++)
        body[17 + i] = 0xff00ff00u >> (8 * i);
    assert(data_settings_decode(body, used, &l) == -EBADMSG);
    for (unsigned i = 0; i < 4; i++)
        body[17 + i] = 0xffffffffu >> (8 * i);
    memset(body + 3, 0, 4);
    assert(data_settings_decode(body, used, &l) == -EBADMSG);
    for (unsigned i = 0; i < 4; i++)
        body[3 + i] = 0x0a141e28u >> (8 * i);
    uint8_t duplicated[128];
    memcpy(duplicated, body, used);
    memcpy(duplicated + used, body, 7);
    assert(data_settings_decode(duplicated, used + 7, &l) == -EBADMSG);
    unsigned seed = 1;
    for (unsigned round = 0; round < 65536; round++) {
        uint8_t fuzz[128];
        size_t bytes = round % sizeof(fuzz);
        for (size_t i = 0; i < bytes; i++) {
            seed = seed * 1664525u + 1013904223u;
            fuzz[i] = seed >> 24;
        }
        (void)data_handle_decode(fuzz, bytes, &h);
        (void)data_packet_status(fuzz, bytes);
        (void)data_settings_decode(fuzz, bytes, &l);
    }
    puts("PASS actual data profile/start payload/handle/status/IPv4 mask+endianness/MTU and65536 "
         "malformed responses; no fabricated connection values");
}

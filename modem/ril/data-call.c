// SPDX-License-Identifier: Apache-2.0
// Legacy WDS protocol fields, never a persistent modem-profile writer.
#include "data-call.h"
#include "../qmux-codec.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <telephony/ril.h>
static int integer(const char *text, unsigned limit, unsigned *value) {
    if (!text || !*text)
        return -EINVAL;
    unsigned v = 0;
    for (unsigned i = 0; i < 6; i++) {
        if (!text[i]) {
            *value = v;
            return 0;
        }
        if (text[i] < '0' || text[i] > '9')
            return -EINVAL;
        unsigned digit = text[i] - '0';
        if (v > limit / 10 || (v == limit / 10 && digit > limit % 10))
            return -EINVAL;
        v = 10 * v + digit;
    }
    return -EINVAL;
}
static int text_copy(char *out, size_t capacity, const char *in) {
    if (!in) {
        out[0] = 0;
        return 0;
    }
    size_t n = strnlen(in, capacity);
    if (n >= capacity)
        return -EINVAL;
    memcpy(out, in, n + 1);
    return 0;
}
static int apn_valid(const char *apn) {
    unsigned label = 0;
    int hyphen = 0;
    if (!*apn)
        return 1;
    for (const unsigned char *p = (const unsigned char *)apn; *p; p++) {
        if (*p == '.') {
            if (!label || hyphen)
                return 0;
            label = 0;
            continue;
        }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return 0;
        if (!label && *p == '-')
            return 0;
        if (++label > 63)
            return 0;
        hyphen = *p == '-';
    }
    return label && !hyphen;
}
int data_profile_parse(const void *data, size_t bytes, struct data_profile *profile) {
    if (!data || bytes != 7 * sizeof(char *) || !profile)
        return -EINVAL;
    const char *const *args = data;
    unsigned technology, ignored;
    if (integer(args[0], 32, &technology) || technology < 2 || integer(args[1], 65535, &ignored))
        return -EINVAL;
    switch (technology - 2) {
    case RADIO_TECH_UNKNOWN:
    case RADIO_TECH_GPRS:
    case RADIO_TECH_EDGE:
    case RADIO_TECH_UMTS:
    case RADIO_TECH_HSDPA:
    case RADIO_TECH_HSUPA:
    case RADIO_TECH_HSPA:
    case RADIO_TECH_HSPAP:
    case RADIO_TECH_GSM:
        break;
    default:
        return -ENOTSUP;
    }
    if (!args[6] || (strcmp(args[6], "IP") && strcmp(args[6], "IPV4V6")))
        return -ENOTSUP;
    struct data_profile p = {0};
    if (text_copy(p.apn, sizeof(p.apn), args[2]) || !apn_valid(p.apn) ||
        text_copy(p.user, sizeof(p.user), args[3]) ||
        text_copy(p.password, sizeof(p.password), args[4]) ||
        integer(args[5], 3, &p.authentication))
        return -EINVAL;
    *profile = p;
    return 0;
}
static int append(uint8_t *out, size_t capacity, size_t *used, unsigned type, const void *value,
                  size_t bytes) {
    if (bytes > 65535 || *used > capacity || capacity - *used < 3 + bytes)
        return -ENOSPC;
    out[*used] = type;
    qmux_write16(out + *used + 1, bytes);
    if (bytes)
        memcpy(out + *used + 3, value, bytes);
    *used += 3 + bytes;
    return 0;
}
int data_start_request(uint8_t *out, size_t capacity, const struct data_profile *p) {
    if (!out || !p || p->authentication > 3)
        return -EINVAL;
    size_t apn = strnlen(p->apn, sizeof(p->apn)), user = strnlen(p->user, sizeof(p->user)),
           pass = strnlen(p->password, sizeof(p->password));
    if (apn == sizeof(p->apn) || user == sizeof(p->user) || pass == sizeof(p->password) ||
        !apn_valid(p->apn))
        return -EINVAL;
    size_t used = 0;
    uint8_t auth = p->authentication, family = 4, technology = 1, autoconnect = 0;
    int rc;
    if (apn && (rc = append(out, capacity, &used, 0x14, p->apn, apn)))
        return rc;
    if ((rc = append(out, capacity, &used, 0x16, &auth, 1)))
        return rc;
    if (user && (rc = append(out, capacity, &used, 0x17, p->user, user)))
        return rc;
    if (pass && (rc = append(out, capacity, &used, 0x18, p->password, pass)))
        return rc;
    if ((rc = append(out, capacity, &used, 0x19, &family, 1)) ||
        (rc = append(out, capacity, &used, 0x30, &technology, 1)) ||
        (rc = append(out, capacity, &used, 0x33, &autoconnect, 1)))
        return rc;
    return used;
}
static int value32(const uint8_t *body, size_t bytes, unsigned type, uint32_t *value,
                   int required) {
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, type, &n);
    if (!v) {
        *value = 0;
        return required ? -EBADMSG : 0;
    }
    if (n != 4)
        return -EBADMSG;
    *value =
        (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    return 0;
}
int data_handle_decode(const uint8_t *body, size_t bytes, uint32_t *handle) {
    uint32_t value;
    if (!handle || !qmux_tlvs_valid(body, bytes) || value32(body, bytes, 1, &value, 1) || !value ||
        value == UINT32_MAX)
        return -EBADMSG;
    *handle = value;
    return 0;
}
int data_packet_status(const uint8_t *body, size_t bytes) {
    size_t n;
    if (!qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &n);
    if (!v || (n != 1 && n != 2) || v[0] < 1 || v[0] > 4 || (n == 2 && v[1] > 1))
        return -EBADMSG;
    return v[0];
}
static int unicast(uint32_t address) {
    unsigned first = address >> 24;
    return first && first != 127 && first < 224;
}
int data_settings_decode(const uint8_t *body, size_t bytes, struct data_lease *lease) {
    if (!lease || !qmux_tlvs_valid(body, bytes))
        return -EBADMSG;
    struct data_lease l = {0};
    uint32_t mtu;
    if (value32(body, bytes, 0x1e, &l.address, 1) || value32(body, bytes, 0x20, &l.gateway, 1) ||
        value32(body, bytes, 0x21, &l.mask, 1) || value32(body, bytes, 0x15, &l.dns[0], 0) ||
        value32(body, bytes, 0x16, &l.dns[1], 0) || value32(body, bytes, 0x29, &mtu, 0))
        return -EBADMSG;
    if (!unicast(l.address) || !l.mask || (l.gateway && !unicast(l.gateway)) ||
        (l.dns[0] && !unicast(l.dns[0])) || (l.dns[1] && !unicast(l.dns[1])))
        return -EBADMSG;
    uint32_t host = ~l.mask;
    if (host & (host + 1))
        return -EBADMSG;
    for (uint32_t bit = 0x80000000u; l.mask & bit; bit >>= 1) {
        l.prefix++;
        if (!bit)
            break;
    }
    l.mtu = mtu ? mtu : 1500;
    if (l.mtu < 576)
        return -EBADMSG;
    if (l.mtu > 1500)
        l.mtu = 1500;
    *lease = l;
    return 0;
}
void data_ipv4_string(uint32_t address, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", address >> 24, (address >> 16) & 255, (address >> 8) & 255,
             address & 255);
}

// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_DATA_CALL_H
#define SUPERMAN_RIL_DATA_CALL_H
#include <stddef.h>
#include <stdint.h>
struct data_profile {
    char apn[101], user[128], password[128];
    unsigned authentication;
};
struct data_lease {
    uint32_t address, mask, gateway, dns[2];
    unsigned prefix, mtu;
};
int data_profile_parse(const void *data, size_t bytes, struct data_profile *profile);
int data_start_request(uint8_t *out, size_t capacity, const struct data_profile *profile);
int data_handle_decode(const uint8_t *body, size_t bytes, uint32_t *handle);
int data_packet_status(const uint8_t *body, size_t bytes);
int data_settings_decode(const uint8_t *body, size_t bytes, struct data_lease *lease);
void data_ipv4_string(uint32_t address, char out[16]);
#endif

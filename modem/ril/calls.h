// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_CALLS_H
#define SUPERMAN_RIL_CALLS_H
#include <stddef.h>
#include <stdint.h>
#include <telephony/ril.h>
#define SUPERMAN_MAX_CALLS 16
struct call_list {
    RIL_Call calls[SUPERMAN_MAX_CALLS];
    RIL_Call *pointers[SUPERMAN_MAX_CALLS];
    char numbers[SUPERMAN_MAX_CALLS][82];
    unsigned count;
};
int dial_request(uint8_t *out, size_t capacity, const char *number);
int calls_decode(const uint8_t *body, size_t bytes, struct call_list *out);
#endif

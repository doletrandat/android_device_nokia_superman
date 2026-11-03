// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_SUBSCRIPTION_H
#define SUPERMAN_RIL_SUBSCRIPTION_H
#include <stddef.h>
#include <stdint.h>
struct subscription_state {
    uint8_t standby, priority, active, data, voice;
};
int subscription_decode(const uint8_t *tlvs, size_t bytes, struct subscription_state *out);
int subscription_request(uint8_t *out, size_t capacity, const struct subscription_state *state);
#endif

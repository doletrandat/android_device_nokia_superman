// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_MODEL_H
#define SUPERMAN_RIL_MODEL_H
#include <stddef.h>
#include <stdint.h>
#include <telephony/ril.h>
struct sim_status {
    RIL_CardStatus_v6 card;
    char aid[RIL_CARD_MAX_APPS][65];
    uint8_t raw_aid[RIL_CARD_MAX_APPS][32];
    uint8_t aid_bytes[RIL_CARD_MAX_APPS];
    uint16_t provisioning_index;
};
struct serving_status {
    int registration, cs, ps, technology;
    int lac;
    uint32_t cell;
    int has_lac, has_cell;
};
int sim_decode(const uint8_t *data, size_t bytes, unsigned slot, unsigned subscription,
               struct sim_status *status);
int serving_decode(const uint8_t *tlvs, size_t bytes, struct serving_status *status);
int bytes_from_hex(const char *text, uint8_t *bytes, size_t capacity);
void bytes_to_hex(const uint8_t *bytes, size_t count, char *text);
#endif

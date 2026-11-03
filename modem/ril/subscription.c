// SPDX-License-Identifier: Apache-2.0
#include "subscription.h"
#include "../qmux-codec.h"
#include <errno.h>
static int standby_valid(unsigned mode) {
    return mode == 1 || mode == 2 || (mode >= 4 && mode <= 7);
}
int subscription_decode(const uint8_t *body, size_t bytes, struct subscription_state *out) {
    uint8_t fields[5];
    for (unsigned i = 0; i < 5; i++) {
        size_t n;
        const uint8_t *v = qmux_tlv(body, bytes, 0x10 + i, &n);
        if (!v || n != 1)
            return -EBADMSG;
        fields[i] = v[0];
    }
    if (!standby_valid(fields[0]) || fields[1] > 2 || fields[2] > 2 || fields[3] > 2 ||
        fields[4] > 2)
        return -EBADMSG;
    *out = (struct subscription_state){fields[0], fields[1], fields[2], fields[3], fields[4]};
    return 0;
}
int subscription_request(uint8_t *out, size_t capacity, const struct subscription_state *state) {
    if (!state || !standby_valid(state->standby) || state->priority > 2 || state->data > 2 ||
        state->voice > 2)
        return -EINVAL;
    if (capacity < 12)
        return -ENOSPC;
    // This firmware reports standby=1 but rejects that value in the setter. Preserve it.
    uint8_t fields[] = {state->priority, state->data, state->voice};
    for (unsigned i = 0; i < 3; i++) {
        out[4 * i] = 0x11 + i;
        out[4 * i + 1] = 1;
        out[4 * i + 2] = 0;
        out[4 * i + 3] = fields[i];
    }
    return 12;
}

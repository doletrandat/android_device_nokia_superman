// SPDX-License-Identifier: Apache-2.0
#include "calls.h"
#include "../qmux-codec.h"
#include <errno.h>
#include <string.h>
int calls_decode(const uint8_t *body, size_t bytes, struct call_list *out) {
    memset(out, 0, sizeof(*out));
    size_t len;
    const uint8_t *v = qmux_tlv(body, bytes, 0x10, &len);
    if (!v || len < 1 || v[0] > SUPERMAN_MAX_CALLS || len != 1 + (size_t)v[0] * 7)
        return -EBADMSG;
    unsigned seen[256] = {0};
    for (unsigned i = 0; i < v[0]; i++) {
        const uint8_t *c = v + 1 + i * 7;
        if (!c[0] || seen[c[0]]++ || c[1] < 1 || c[1] > 10 || c[3] < 1 || c[3] > 2 || c[5] > 1)
            return -EBADMSG;
        if (c[1] == 8 || c[1] == 9)
            continue;
        RIL_Call *call = &out->calls[out->count];
        static const RIL_CallState states[] = {
            RIL_CALL_ACTIVE,  RIL_CALL_DIALING,  RIL_CALL_INCOMING, RIL_CALL_ACTIVE,
            RIL_CALL_DIALING, RIL_CALL_ALERTING, RIL_CALL_HOLDING,  RIL_CALL_WAITING,
            RIL_CALL_ACTIVE,  RIL_CALL_ACTIVE,   RIL_CALL_INCOMING};
        call->state = states[c[1]];
        call->index = c[0];
        call->isMT = c[3] == 2;
        call->isMpty = c[5];
        call->isVoice = c[2] == 0 || c[2] == 1 || c[2] == 9;
        call->als = c[6];
        call->toa = 129;
        call->numberPresentation = 2;
        call->namePresentation = 2;
        out->pointers[out->count++] = call;
    }
    v = qmux_tlv(body, bytes, 0x11, &len);
    if (!v)
        return 0;
    if (len < 1 || v[0] > SUPERMAN_MAX_CALLS)
        return -EBADMSG;
    size_t at = 1;
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < v[0]; i++) {
        if (len - at < 3)
            return -EBADMSG;
        unsigned id = v[at++], presentation = v[at++], n = v[at++];
        if (!id || seen[id]++ || presentation > 3 || n > 81 || n > len - at)
            return -EBADMSG;
        for (unsigned j = 0; j < n; j++)
            if (!((v[at + j] >= '0' && v[at + j] <= '9') || v[at + j] == '+' || v[at + j] == '*' ||
                  v[at + j] == '#'))
                return -EBADMSG;
        for (unsigned j = 0; j < out->count; j++)
            if (out->calls[j].index == (int)id) {
                out->calls[j].numberPresentation = presentation;
                // Never expose a number the modem marks as restricted or unavailable.
                if (!presentation && n) {
                    memcpy(out->numbers[j], v + at, n);
                    out->calls[j].number = out->numbers[j];
                    out->calls[j].toa = out->numbers[j][0] == '+' ? 145 : 129;
                }
            }
        at += n;
    }
    return at == len ? 0 : -EBADMSG;
}

int dial_request(uint8_t *out, size_t capacity, const char *number) {
    if (!number)
        return -EINVAL;
    size_t n = strnlen(number, 82);
    if (!n || n > 81 || (number[0] == '+' && n == 1))
        return -EINVAL;
    for (size_t i = 0; i < n; i++)
        if (!((number[i] >= '0' && number[i] <= '9') || number[i] == '*' || number[i] == '#' ||
              (i == 0 && number[i] == '+')))
            return -EINVAL;
    if (capacity < n + 3)
        return -ENOSPC;
    out[0] = 1;
    qmux_write16(out + 1, n);
    memcpy(out + 3, number, n);
    return n + 3;
}

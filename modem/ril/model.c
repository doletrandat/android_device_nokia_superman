// SPDX-License-Identifier: Apache-2.0
#include "model.h"
#include "../qmux-codec.h"
#include <errno.h>
#include <string.h>
static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
int bytes_from_hex(const char *text, uint8_t *out, size_t capacity) {
    if (!text)
        return -EINVAL;
    size_t len = strnlen(text, capacity * 2 + 1);
    if (len > capacity * 2 || len % 2)
        return -EINVAL;
    for (size_t i = 0; i < len; i += 2) {
        int a = nibble(text[i]), b = nibble(text[i + 1]);
        if (a < 0 || b < 0)
            return -EINVAL;
        out[i / 2] = (a << 4) | b;
    }
    return len / 2;
}
void bytes_to_hex(const uint8_t *bytes, size_t count, char *text) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < count; i++) {
        text[2 * i] = digits[bytes[i] >> 4];
        text[2 * i + 1] = digits[bytes[i] & 15];
    }
    text[2 * count] = 0;
}
static RIL_PinState pin(unsigned state) {
    return state <= 5 ? (RIL_PinState)state : RIL_PINSTATE_UNKNOWN;
}
static RIL_PersoSubstate perso(unsigned state, unsigned feature) {
    if (state <= 2)
        return (RIL_PersoSubstate)state;
    static const RIL_PersoSubstate required[] = {
        RIL_PERSOSUBSTATE_SIM_NETWORK, RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET,
        RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER, RIL_PERSOSUBSTATE_SIM_CORPORATE,
        RIL_PERSOSUBSTATE_SIM_SIM};
    if ((state == 3 || state == 4) && feature < 5)
        return (RIL_PersoSubstate)(required[feature] + (state == 4 ? 5 : 0));
    return RIL_PERSOSUBSTATE_UNKNOWN;
}
static RIL_AppState app_state(unsigned state) {
    switch (state) {
    case 1:
        return RIL_APPSTATE_DETECTED;
    case 2:
        return RIL_APPSTATE_PIN;
    case 3:
    case 5:
        return RIL_APPSTATE_PUK;
    case 4:
        return RIL_APPSTATE_SUBSCRIPTION_PERSO;
    case 7:
        return RIL_APPSTATE_READY;
    default:
        return RIL_APPSTATE_UNKNOWN;
    }
}
int sim_decode(const uint8_t *data, size_t bytes, unsigned slot, unsigned subscription,
               struct sim_status *out) {
    if (!data || bytes < 9 || slot >= data[8] || slot > 1 || subscription > 1)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->card.gsm_umts_subscription_app_index = -1;
    out->card.cdma_subscription_app_index = -1;
    out->card.ims_subscription_app_index = -1;
    out->provisioning_index = qmux_read16(data + subscription * 4);
    size_t off = 9;
    for (unsigned s = 0; s < data[8]; s++) {
        if (bytes - off < 6)
            return -EINVAL;
        unsigned card = data[off], apps = data[off + 5];
        if (s == slot) {
            out->card.card_state = card <= 2 ? (RIL_CardState)card : RIL_CARDSTATE_ERROR;
            out->card.universal_pin_state = pin(data[off + 1]);
            if (apps > RIL_CARD_MAX_APPS)
                return -E2BIG;
            out->card.num_applications = apps;
        }
        off += 6;
        for (unsigned a = 0; a < apps; a++) {
            if (bytes - off < 7)
                return -EINVAL;
            unsigned aid = data[off + 6];
            if (aid > 32 || bytes - off < 14 + (size_t)aid)
                return -EINVAL;
            if (s == slot) {
                RIL_AppStatus *app = &out->card.applications[a];
                app->app_type = data[off] <= 5 ? (RIL_AppType)data[off] : RIL_APPTYPE_UNKNOWN;
                app->app_state = app_state(data[off + 1]);
                app->perso_substate = perso(data[off + 2], data[off + 3]);
                out->aid_bytes[a] = aid;
                memcpy(out->raw_aid[a], data + off + 7, aid);
                if (aid) {
                    bytes_to_hex(data + off + 7, aid, out->aid[a]);
                    app->aid_ptr = out->aid[a];
                }
                const uint8_t *p = data + off + 7 + aid;
                app->pin1_replaced = p[0] != 0;
                app->pin1 = pin(p[1]);
                app->pin2 = pin(p[4]);
                // Superman firmware keeps app state at PERSO after personalization is READY.
                // An active session with verified/disabled PIN is readable (PF30 EF tests).
                RIL_PinState effective =
                    app->pin1_replaced ? out->card.universal_pin_state : app->pin1;
                if (data[off + 1] == 4 && data[off + 2] == 2 &&
                    out->provisioning_index == (uint16_t)((slot << 8) | a) &&
                    (effective == RIL_PINSTATE_ENABLED_VERIFIED ||
                     effective == RIL_PINSTATE_DISABLED))
                    app->app_state = RIL_APPSTATE_READY;
                if (out->provisioning_index == (uint16_t)((slot << 8) | a))
                    out->card.gsm_umts_subscription_app_index = a;
            }
            off += 14 + aid;
        }
    }
    return off == bytes ? 0 : -EINVAL;
}
int serving_decode(const uint8_t *body, size_t bytes, struct serving_status *out) {
    size_t len = 0;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    if (!v || len < 5 || len != 5 + (size_t)v[4] || v[0] > 4)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->registration = v[0];
    out->cs = v[1];
    out->ps = v[2];
    for (unsigned i = 0; i < v[4]; i++) {
        if (v[5 + i] == 4)
            out->technology = RADIO_TECH_GSM;
        if (v[5 + i] == 5)
            out->technology = RADIO_TECH_UMTS;
        if (v[5 + i] == 8)
            out->technology = RADIO_TECH_LTE;
    }
    v = qmux_tlv(body, bytes, 0x10, &len);
    if (v && len == 1 && out->registration == 1 && v[0] == 0)
        out->registration = RIL_REG_ROAMING;
    v = qmux_tlv(body, bytes, 0x1a, &len);
    if (v && len == 2) {
        out->lac = qmux_read16(v);
        out->has_lac = 1;
    }
    v = qmux_tlv(body, bytes, 0x1b, &len);
    if (v && len == 4) {
        out->cell = qmux_read16(v) | ((uint32_t)qmux_read16(v + 2) << 16);
        out->has_cell = 1;
    }
    return 0;
}

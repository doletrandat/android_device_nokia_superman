// SPDX-License-Identifier: Apache-2.0
#include "../../qmux-codec.h"
#include "../calls.h"
#include "../model.h"
#include "../sim-io.h"
#include "../subscription.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t card[35] = {0};
    memset(card, 0xff, 8);
    card[8] = 2;
    card[9] = 2;
    card[13] = 3;
    card[15] = 1;
    card[20] = 1;
    card[21] = 1;
    card[22] = 4;
    card[23] = 2;
    card[24] = 11;
    card[29] = 3;
    card[32] = 1;
    struct sim_status status;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) == 0);
    assert(status.card.card_state == RIL_CARDSTATE_PRESENT);
    assert(status.card.num_applications == 1);
    assert(status.card.gsm_umts_subscription_app_index == -1);
    assert(status.card.applications[0].app_type == RIL_APPTYPE_SIM);
    assert(status.card.applications[0].perso_substate == RIL_PERSOSUBSTATE_READY);
    assert(status.card.applications[0].pin1 == RIL_PINSTATE_DISABLED);
    assert(status.card.applications[0].pin2 == RIL_PINSTATE_ENABLED_NOT_VERIFIED);
    for (size_t n = 0; n < sizeof(card); n++)
        assert(sim_decode(card, n, 1, 1, &status) < 0);
    assert(sim_decode(card, sizeof(card), 2, 1, &status) < 0);
    card[4] = 0;
    card[5] = 1;
    card[22] = 4;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) == 0 &&
           status.card.applications[0].app_state == RIL_APPSTATE_READY);
    card[29] = 1;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) == 0 &&
           status.card.applications[0].app_state != RIL_APPSTATE_READY);
    card[29] = 3;
    card[22] = 7;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) == 0);
    assert(status.card.gsm_umts_subscription_app_index == 0);
    assert(status.card.applications[0].app_state == RIL_APPSTATE_READY);
    card[27] = 33;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) < 0);
    card[27] = 0;
    card[20] = 9;
    assert(sim_decode(card, sizeof(card), 1, 1, &status) < 0);
    card[20] = 1;
    // Physical SIM2 can provision either logical subscription; never infer one from the other.
    card[0] = 0;
    card[1] = 1;
    card[4] = card[5] = 0xff;
    card[22] = 4;
    assert(sim_decode(card, sizeof(card), 1, 0, &status) == 0 &&
           status.card.gsm_umts_subscription_app_index == 0 &&
           status.card.applications[0].app_state == RIL_APPSTATE_READY);
    assert(sim_decode(card, sizeof(card), 1, 1, &status) == 0 &&
           status.card.gsm_umts_subscription_app_index == -1 &&
           status.card.applications[0].app_state != RIL_APPSTATE_READY);
    assert(sim_decode(card, sizeof(card), 1, 2, &status) < 0);
    card[1] = 0;
    assert(sim_decode(card, sizeof(card), 1, 0, &status) == 0 &&
           status.card.gsm_umts_subscription_app_index == -1);
    uint8_t input[] = {1, 6, 0, 1, 1, 1, 0, 1, 5, 0x10, 1, 0, 0};
    struct serving_status serving;
    assert(serving_decode(input, sizeof(input), &serving) == 0);
    assert(serving.registration == RIL_REG_ROAMING && serving.technology == RADIO_TECH_UMTS);
    input[4] = 2;
    assert(serving_decode(input, sizeof(input), &serving) == 0 && serving.cs == 2);
    input[7] = 2;
    assert(serving_decode(input, sizeof(input), &serving) < 0);
    uint8_t bytes[8];
    char hex[17];
    assert(bytes_from_hex("00aBff", bytes, sizeof(bytes)) == 3);
    bytes_to_hex(bytes, 3, hex);
    assert(!strcmp(hex, "00ABFF"));
    assert(bytes_from_hex("0x", bytes, sizeof(bytes)) < 0);
    assert(bytes_from_hex("1", bytes, sizeof(bytes)) < 0);
    assert(bytes_from_hex("001122334455667788", bytes, sizeof(bytes)) < 0);
    // Exact cold-modem QMI CTL SYNC indication: it must not fail the pending DMS query.
    uint8_t sync_indication[] = {1, 11, 0, 0x80, 0, 0, 2, 0, 0x27, 0, 0, 0};
    const uint8_t *ignored;
    size_t ignored_bytes;
    assert(qmux_response(sync_indication, sizeof(sync_indication), 2, 1, 8, 0x2d, &ignored,
                         &ignored_bytes) == -2);
    for (size_t n = 0; n < sizeof(sync_indication); n++)
        assert(qmux_response(sync_indication, n, 2, 1, 8, 0x2d, &ignored, &ignored_bytes) == -1);
    uint8_t response[] = {1, 19, 0, 0x80, 2, 3, 2, 1, 0, 0x2d, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    const uint8_t *body;
    size_t len;
    assert(qmux_response(response, sizeof(response), 2, 3, 1, 0x2d, &body, &len) == 0);
    for (size_t n = 0; n < sizeof(response); n++)
        assert(qmux_response(response, n, 2, 3, 1, 0x2d, &body, &len) < 0);
    assert(qmux_response(response, sizeof(response), 2, 4, 1, 0x2d, &body, &len) == -2);
    response[17] = 1;
    assert(qmux_response(response, sizeof(response), 2, 3, 1, 0x2d, &body, &len) == 1);
    uint8_t command[80];
    int command_size =
        uim_file_request(command, sizeof(command), 1, NULL, 0, 0xb0, 0x6f07, "3F007F20", 0, 0, 9);
    assert(command_size == 22 && command[3] == 2 && command[4] == 0);
    assert(command[8] == 7 && command[9] == 0x6f && command[10] == 4);
    assert(!memcmp(command + 11, "\x00\x3f\x20\x7f", 4));
    assert(uim_file_request(command, sizeof(command), 0, NULL, 0, 0xb0, 0x6f07, "3F007F20", 0, 0,
                            9) > 0 &&
           command[3] == 0);
    assert(uim_file_request(command, sizeof(command), 1, NULL, 0, 0xd6, 0x6f07, "3F007F20", 0, 0,
                            9) < 0);
    assert(uim_file_request(command, sizeof(command), 1, NULL, 0, 0xb0, 0x6f07, "../", 0, 0, 9) <
           0);
    assert(uim_file_request(command, 10, 1, NULL, 0, 0xb0, 0x6f07, "3F007F20", 0, 0, 9) < 0);
    assert(uim_file_request(command, sizeof(command), 1, NULL, 0, 0xb2, 0x6f07, "3F007F20", 1, 3,
                            9) < 0);
    uint8_t read_result[] = {0x10, 2, 0, 0x90, 0, 0x11, 5, 0, 3, 0, 0x12, 0x34, 0x56};
    int sw1, sw2;
    const uint8_t *contents;
    size_t count;
    assert(uim_file_response(read_result, sizeof(read_result), 0xb0, 0x6f07, &contents, &count,
                             &sw1, &sw2) == 0);
    assert(sw1 == 0x90 && sw2 == 0 && count == 3 && contents[0] == 0x12);
    read_result[8] = 4;
    assert(uim_file_response(read_result, sizeof(read_result), 0xb0, 0x6f07, &contents, &count,
                             &sw1, &sw2) < 0);
    uint8_t identity[] = {8, 0x19, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32, 0x54};
    char imsi[16];
    assert(imsi_decode(identity, sizeof(identity), imsi) == 15 && !strcmp(imsi, "123456789012345"));
    identity[8] = 0x5a;
    assert(imsi_decode(identity, sizeof(identity), imsi) < 0);
    struct subscription_state subscription;
    uint8_t prefs[] = {0x10, 1, 0, 1, 0x11, 1, 0, 0, 0x12, 1, 0, 0, 0x13, 1, 0, 0, 0x14, 1, 0, 0};
    assert(!subscription_decode(prefs, sizeof(prefs), &subscription));
    assert(subscription.standby == 1 && subscription.priority == 0 && subscription.active == 0);
    for (size_t i = 0; i < sizeof(prefs); i++)
        assert(subscription_decode(prefs, i, &subscription) < 0);
    assert(!subscription_decode(prefs, sizeof(prefs), &subscription));
    subscription.priority = subscription.data = subscription.voice = 1;
    uint8_t selection[16];
    assert(subscription_request(selection, sizeof(selection), &subscription) == 12);
    assert(selection[0] == 0x11 && selection[3] == 1 && selection[4] == 0x12 && selection[7] == 1);
    assert(selection[8] == 0x13 && selection[11] == 1);
    assert(subscription_request(selection, 11, &subscription) < 0);
    subscription.data = 3;
    assert(subscription_request(selection, sizeof(selection), &subscription) < 0);
    uint8_t dial[90];
    assert(dial_request(dial, sizeof(dial), "+12345") == 9);
    assert(dial[0] == 1 && dial[1] == 6 && !memcmp(dial + 3, "+12345", 6));
    assert(dial_request(dial, 8, "+12345") < 0);
    assert(dial_request(dial, sizeof(dial), NULL) < 0);
    assert(dial_request(dial, sizeof(dial), "") < 0);
    assert(dial_request(dial, sizeof(dial), "+") < 0);
    assert(dial_request(dial, sizeof(dial), "1+2") < 0);
    assert(dial_request(dial, sizeof(dial), "123;AT") < 0);
    struct call_list calls;
    uint8_t empty_calls[] = {0x10, 1, 0, 0};
    assert(calls_decode(empty_calls, sizeof(empty_calls), &calls) == 0 && !calls.count);
    uint8_t call_body[] = {0x10, 8, 0, 1, 2, 2, 0, 2,   2,   0,  0,
                           0x11, 7, 0, 1, 2, 0, 3, '+', '1', '2'};
    assert(!calls_decode(call_body, sizeof(call_body), &calls));
    assert(calls.count == 1 && calls.calls[0].state == RIL_CALL_INCOMING && calls.calls[0].isMT);
    assert(calls.calls[0].toa == 145 && !strcmp(calls.calls[0].number, "+12"));
    call_body[16] = 1;
    assert(!calls_decode(call_body, sizeof(call_body), &calls) && !calls.calls[0].number);
    for (size_t n = 0; n < sizeof(call_body); n++)
        if (n != 11)
            assert(calls_decode(call_body, n, &calls) < 0);
    call_body[5] = 11;
    assert(calls_decode(call_body, sizeof(call_body), &calls) < 0);
    puts("PASS shared QMUX codec, physical SIM2/card/application mapping, NAS registration and "
         "malformed/overflow cases");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_SMS_H
#define SUPERMAN_RIL_SMS_H
#include <stddef.h>
#include <stdint.h>
#define SUPERMAN_SMS_MAX_PDU 256
#define SUPERMAN_SMS_QUEUE 8
struct sms_event {
    uint32_t transaction;
    int ack_required, ims, status_report, sim_index;
    char pdu[2 * SUPERMAN_SMS_MAX_PDU + 1];
};
struct sms_queue {
    struct sms_event events[SUPERMAN_SMS_QUEUE];
    unsigned head, count;
    int delivered;
};
int sms_send_request(uint8_t *out, size_t capacity, const char *smsc, const char *tpdu);
int sms_send_response(const uint8_t *body, size_t bytes, int *reference);
int sms_smsc_response(const uint8_t *body, size_t bytes, char *out, size_t capacity);
int sms_routes_transfer(const uint8_t *body, size_t bytes);
/* One supported event, zero unrelated event, negative malformed/unsupported. */
int sms_event_decode(const uint8_t *body, size_t bytes, struct sms_event *event);
int sms_queue_push(struct sms_queue *queue, const struct sms_event *event);
void sms_queue_pop(struct sms_queue *queue);
int sms_ack_request(uint8_t *out, size_t capacity, const struct sms_event *event, int success,
                    int cause);
#endif

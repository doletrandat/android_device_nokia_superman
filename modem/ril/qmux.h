// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_QMUX_H
#define SUPERMAN_RIL_QMUX_H
#include <stddef.h>
#include <stdint.h>
typedef void (*qmux_async_callback)(int result, const uint8_t *body, size_t bytes);
struct qmux {
    int fd, control;
    uint16_t transaction;
    uint8_t clients[256];
    uint8_t response[4096];
    char endpoint[128];
    uint8_t async_service, async_client;
    uint16_t async_transaction, async_message;
    qmux_async_callback async_callback;
    /* Callback borrows the frame until the next read; must not issue requests. */
    void (*indication)(uint8_t service, uint16_t message, const uint8_t *body, size_t bytes);
};
int qmux_open(struct qmux *q);
void qmux_close(struct qmux *q);
/* Zero: modem success. Positive: exact QMI error. Negative: transport/format error. */
int qmux_request(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                 const void *tlvs, size_t size, const uint8_t **response, size_t *bytes);
int qmux_client(struct qmux *q, uint8_t service);
/* Drain at most16 available frames without blocking; callers serialize transport. */
int qmux_poll(struct qmux *q);
/* One pending long operation. Callback only copies state; never issue requests.
 * Caller owns its deadline/cancellation and completes it before closing q. */
int qmux_send_async(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                    const void *tlvs, size_t size, qmux_async_callback callback);
int qmux_release_client(struct qmux *q, uint8_t service);
#endif

// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../qmux.c"
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
static unsigned indications, completions;
static int last_result;
static uint8_t saved[64];
static size_t saved_bytes;
int __android_log_print(int p, const char *t, const char *f, ...) {
    (void)p;
    (void)t;
    (void)f;
    return 0;
}
static void indication(uint8_t service, uint16_t message, const uint8_t *body, size_t bytes) {
    assert(service == 5 && message == 1 && bytes == 4 && body[0] == 0x10);
    indications++;
}
static void complete(int result, const uint8_t *body, size_t bytes) {
    assert(bytes <= sizeof(saved));
    last_result = result;
    memcpy(saved, body, bytes);
    saved_bytes = bytes;
    completions++;
}
static void frame(int fd, uint8_t service, uint8_t client, uint8_t flag, uint16_t txn,
                  uint16_t message, const void *data, size_t bytes) {
    uint8_t raw[128] = {1};
    size_t head = service ? 13 : 12;
    assert(bytes + head <= sizeof(raw));
    qmux_write16(raw + 1, head + bytes - 1);
    raw[3] = 0x80;
    raw[4] = service;
    raw[5] = client;
    raw[6] = flag;
    if (service)
        qmux_write16(raw + 7, txn);
    else
        raw[7] = txn;
    qmux_write16(raw + (service ? 9 : 8), message);
    qmux_write16(raw + (service ? 11 : 10), bytes);
    if (bytes)
        memcpy(raw + head, data, bytes);
    assert(write(fd, raw, head + bytes) == (ssize_t)(head + bytes));
}
static ssize_t receive(int fd, uint8_t *raw, size_t bytes) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 1000) == 1);
    ssize_t n = read(fd, raw, bytes);
    assert(n >= 12);
    return n;
}
static void *modem(void *arg) {
    int fd = *(int *)arg;
    uint8_t raw[128];
    assert(receive(fd, raw, sizeof(raw)) == 13 && raw[4] == 1 && raw[5] == 2 &&
           qmux_read16(raw + 9) == 0x22);
    uint16_t async_txn = qmux_read16(raw + 7);
    assert(receive(fd, raw, sizeof(raw)) == 13 && raw[4] == 2 && raw[5] == 3 &&
           qmux_read16(raw + 9) == 0x2d);
    uint16_t sync_txn = qmux_read16(raw + 7);
    assert(async_txn != sync_txn);
    const uint8_t report[] = {0x10, 1, 0, 1};
    frame(fd, 5, 1, 4, 0, 1, report, sizeof(report));
    const uint8_t data[] = {2, 4, 0, 0, 0, 0, 0, 1, 1, 0, 1};
    frame(fd, 1, 2, 2, async_txn, 0x22, data, sizeof(data));
    frame(fd, 1, 2, 2, async_txn, 0x22, data, sizeof(data));
    const uint8_t mode[] = {2, 4, 0, 0, 0, 0, 0, 1, 1, 0, 0};
    frame(fd, 2, 3, 2, sync_txn, 0x2d, mode, sizeof(mode));
    return NULL;
}
static void *release_modem(void *arg) {
    int fd = *(int *)arg;
    uint8_t raw[128];
    ssize_t n = receive(fd, raw, sizeof(raw));
    assert(n == 17 && raw[4] == 0 && raw[8] == 0x23 && raw[12] == 1 && raw[13] == 2 &&
           raw[15] == 1 && raw[16] == 2);
    uint16_t txn = raw[7];
    const uint8_t result[] = {2, 4, 0, 0, 0, 0, 0};
    frame(fd, 0, 0, 1, txn, 0x23, result, sizeof(result));
    return NULL;
}
int main(void) {
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
    struct qmux q = {.fd = pair[0], .control = -1, .indication = indication};
    q.clients[1] = 2;
    q.clients[2] = 3;
    q.clients[5] = 1;
    pthread_t worker;
    assert(qmux_send_async(&q, 1, 3, 0x22, NULL, 0, complete) == -EINVAL);
    assert(qmux_send_async(&q, 1, 2, 0x22, NULL, 1, complete) == -EINVAL && !q.async_callback);
    assert(qmux_send_async(&q, 1, 2, 0x22, NULL, 0, NULL) == -EINVAL);
    q.transaction = 254;
    assert(!qmux_send_async(&q, 1, 2, 0x22, NULL, 0, complete) && q.async_transaction == 255);
    assert(qmux_send_async(&q, 1, 2, 0x22, NULL, 0, complete) == -EBUSY);
    q.transaction = 254;
    assert(!pthread_create(&worker, NULL, modem, &pair[1]));
    const uint8_t *body;
    size_t bytes, n;
    assert(!qmux_request(&q, 2, 3, 0x2d, NULL, 0, &body, &bytes));
    assert(!pthread_join(worker, NULL));
    assert(indications == 1 && completions == 1 && !last_result && !q.async_callback);
    assert(saved_bytes == 11 && *qmux_tlv(saved, saved_bytes, 1, &n) == 1 && n == 1);
    assert(*qmux_tlv(body, bytes, 1, &n) == 0);
    assert(!qmux_send_async(&q, 1, 2, 0x22, NULL, 0, complete));
    uint8_t raw[128];
    assert(receive(pair[1], raw, sizeof(raw)) == 13);
    uint16_t txn = qmux_read16(raw + 7);
    const uint8_t error[] = {2, 4, 0, 1, 0, 52, 0};
    frame(pair[1], 1, 9, 2, txn, 0x22, error, sizeof(error));
    assert(!qmux_poll(&q) && completions == 1 && q.async_callback);
    frame(pair[1], 1, 2, 2, txn, 0x22, error, sizeof(error));
    assert(!qmux_poll(&q) && completions == 2 && last_result == 52 && !q.async_callback);
    assert(!qmux_send_async(&q, 1, 2, 0x22, NULL, 0, complete));
    assert(receive(pair[1], raw, sizeof(raw)) == 13);
    assert(!pthread_create(&worker, NULL, release_modem, &pair[1]));
    assert(!qmux_release_client(&q, 1));
    assert(!pthread_join(worker, NULL));
    assert(!q.clients[1] && !q.async_callback && q.clients[5] == 1 && q.clients[2] == 3 &&
           completions == 2);
    assert(qmux_release_client(&q, 1) == -EINVAL);
    assert(qmux_release_client(&q, 0) == -EINVAL);
    close(pair[0]);
    close(pair[1]);
    q.fd = -1;
    q.clients[1] = 2;
    assert(qmux_send_async(&q, 1, 2, 0x22, NULL, 0, complete) == -ENODEV && !q.async_callback);
    puts("PASS actual async QMUX: SMS indications and sync replies during pending operation, "
         "exactly-once callback, transaction wrap, foreign replies, owned client release and send "
         "failures");
}

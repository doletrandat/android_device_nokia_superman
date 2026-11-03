// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../qmux.c"
#include "../sms.h"
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
static unsigned received;
static struct sms_event copied;
int __android_log_print(int p, const char *t, const char *f, ...) {
    (void)p;
    (void)t;
    (void)f;
    return 0;
}
static void indication(uint8_t service, uint16_t message, const uint8_t *body, size_t bytes) {
    assert(service == 5 && message == 1);
    assert(sms_event_decode(body, bytes, &copied) == 1);
    received++;
}
static void frame(int fd, uint8_t svc, uint8_t cid, uint8_t flag, uint16_t txn, uint16_t message,
                  const void *data, size_t bytes) {
    uint8_t raw[128] = {1};
    assert(bytes + 13 <= sizeof(raw));
    qmux_write16(raw + 1, 12 + bytes);
    raw[3] = 0x80;
    raw[4] = svc;
    raw[5] = cid;
    raw[6] = flag;
    qmux_write16(raw + 7, txn);
    qmux_write16(raw + 9, message);
    qmux_write16(raw + 11, bytes);
    if (bytes)
        memcpy(raw + 13, data, bytes);
    assert(write(fd, raw, 13 + bytes) == (ssize_t)(13 + bytes));
}
static const uint8_t event[] = {0x11, 11, 0, 0, 0x78, 0x56, 0x34, 0x12, 6, 3, 0, 0, 0, 0};
static void *modem(void *arg) {
    int fd = *(int *)arg;
    uint8_t raw[128];
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 1000) == 1);
    ssize_t n = read(fd, raw, sizeof(raw));
    assert(n == 13 && raw[4] == 5 && raw[5] == 1 && qmux_read16(raw + 9) == 0x34);
    uint16_t txn = qmux_read16(raw + 7);
    const uint8_t ctl[] = {1, 11, 0, 0x80, 0, 0, 2, 0, 0x27, 0, 0, 0};
    assert(write(fd, ctl, sizeof(ctl)) == sizeof(ctl));
    frame(fd, 5, 9, 4, 0, 1, event, sizeof(event));
    frame(fd, 5, 1, 4, 0, 1, event, sizeof(event));
    frame(fd, 5, 255, 4, 0, 1, event, sizeof(event));
    frame(fd, 3, 1, 4, 0, 1, event, sizeof(event));
    const uint8_t result[] = {2, 4, 0, 0, 0, 0, 0};
    frame(fd, 5, 1, 2, txn + 1, 0x34, result, sizeof(result));
    frame(fd, 5, 1, 2, txn, 0x34, result, sizeof(result));
    return NULL;
}
int main(void) {
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
    struct qmux q = {.fd = pair[0], .control = -1, .indication = indication};
    q.clients[5] = 1;
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, modem, &pair[1]));
    const uint8_t *body;
    size_t bytes;
    assert(!qmux_request(&q, 5, 1, 0x34, NULL, 0, &body, &bytes));
    assert(bytes == 7 && body[0] == 2 && received == 2 && copied.transaction == 0x12345678 &&
           !strcmp(copied.pdu, "000000"));
    assert(!pthread_join(worker, NULL));
    for (unsigned i = 0; i < 32; i++)
        frame(pair[1], 5, 1, 4, 0, 1, event, sizeof(event));
    assert(!qmux_poll(&q) && received == 18);
    assert(!qmux_poll(&q) && received == 34);
    assert(!qmux_poll(&q) && received == 34);
    uint8_t bad[sizeof(event)];
    memcpy(bad, event, sizeof(event));
    bad[1]++;
    frame(pair[1], 5, 1, 4, 0, 1, bad, sizeof(bad));
    assert(qmux_poll(&q) == -EBADMSG && received == 34);
    uint8_t duplicate[2 * sizeof(event)];
    memcpy(duplicate, event, sizeof(event));
    memcpy(duplicate + sizeof(event), event, sizeof(event));
    frame(pair[1], 5, 1, 4, 0, 1, duplicate, sizeof(duplicate));
    assert(qmux_poll(&q) == -EBADMSG && received == 34);
    frame(pair[1], 5, 1, 4, 0, 1, event, sizeof(event));
    assert(!qmux_poll(&q) && received == 35);
    close(pair[0]);
    close(pair[1]);
    puts("PASS actual QMUX exchange/poll: CTL12 regression, owned/broadcast indications, unrelated "
         "replies, bounded drain and malformed frames");
}

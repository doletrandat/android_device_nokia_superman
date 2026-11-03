// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "qmux.h"
#include "../qmux-codec.h"
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct endpoint_info {
    char name[32];
    uint32_t source, destination;
};
#define CREATE_ENDPOINT _IOW(0xb5, 1, struct endpoint_info)
#define DESTROY_ENDPOINT _IO(0xb5, 2)
static int64_t milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int read_text(const char *path, char *text, size_t bytes) {
    FILE *f = fopen(path, "re");
    if (!f)
        return -errno;
    char *got = fgets(text, bytes, f);
    fclose(f);
    if (!got)
        return -EIO;
    text[strcspn(text, "\n")] = 0;
    return 0;
}
static int find_port(const char *parent, const char *name, char *device, size_t bytes) {
    DIR *d = opendir("/sys/class/rpmsg");
    if (!d)
        return -errno;
    struct dirent *e;
    int count = 0;
    char path[PATH_MAX], resolved[PATH_MAX], value[64];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/rpmsg/%s", e->d_name);
        if (!realpath(path, resolved) || !strstr(resolved, parent))
            continue;
        if (name) {
            snprintf(path, sizeof(path), "/sys/class/rpmsg/%s/name", e->d_name);
            if (read_text(path, value, sizeof(value)) || strcmp(value, name))
                continue;
        } else if (strncmp(e->d_name, "rpmsg_ctrl", 10))
            continue;
        if (snprintf(device, bytes, "/dev/%s", e->d_name) >= (int)bytes) {
            closedir(d);
            return -ENAMETOOLONG;
        }
        count++;
    }
    closedir(d);
    return count == 1 ? 0 : count ? -EEXIST : -ENOENT;
}
static int open_character(const char *path) {
    struct stat st;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
        close(fd);
        return -EINVAL;
    }
    return fd;
}
static int board_is_superman(void) {
    char compatible[256];
    int fd = open("/sys/firmware/devicetree/base/compatible", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, compatible, sizeof(compatible));
    close(fd);
    int board = 0, soc = 0;
    for (size_t off = 0; n > 0 && off < (size_t)n;) {
        size_t len = strnlen(compatible + off, (size_t)n - off);
        if (len == (size_t)n - off)
            return 0;
        if (!strcmp(compatible + off, "nokia,superman"))
            board = 1;
        if (!strcmp(compatible + off, "qcom,msm8226"))
            soc = 1;
        off += len + 1;
    }
    return board && soc;
}
int qmux_open(struct qmux *q) {
    memset(q, 0, sizeof(*q));
    q->fd = -1;
    q->control = -1;
    if (!board_is_superman())
        return -ENODEV;
    DIR *d = opendir("/sys/class/remoteproc");
    if (!d)
        return -errno;
    struct dirent *e;
    char path[PATH_MAX], text[128], modem[64] = {0};
    int count = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "remoteproc", 10))
            continue;
        snprintf(path, sizeof(path), "/sys/class/remoteproc/%s/firmware", e->d_name);
        if (read_text(path, text, sizeof(text)) || strcmp(text, "qcom/superman/mba-pf29.b00"))
            continue;
        snprintf(path, sizeof(path), "/sys/class/remoteproc/%s/state", e->d_name);
        if (read_text(path, text, sizeof(text)) || strcmp(text, "running"))
            continue;
        if (strlen(e->d_name) >= sizeof(modem)) {
            closedir(d);
            return -EINVAL;
        }
        strcpy(modem, e->d_name);
        count++;
    }
    closedir(d);
    if (count != 1)
        return -ENODEV;
    char parent[PATH_MAX], control[128], sys[PATH_MAX];
    snprintf(parent, sizeof(parent), "/%s:smd-edge.rpmsg_ctrl.0.0/", modem);
    int rc = find_port(parent, NULL, control, sizeof(control));
    if (rc)
        return rc;
    snprintf(sys, sizeof(sys), "/sys/class/rpmsg/%s", strrchr(control, '/') + 1);
    if (!realpath(sys, parent) || strlen(parent) + 1 >= sizeof(parent))
        return -ENAMETOOLONG;
    strcat(parent, "/");
    rc = find_port(parent, "DATA5_CNTL", q->endpoint, sizeof(q->endpoint));
    if (rc != -ENOENT)
        return rc ? rc : -EBUSY;
    q->control = open_character(control);
    if (q->control < 0)
        return q->control;
    struct endpoint_info info = {
        .name = "DATA5_CNTL", .source = UINT32_MAX, .destination = UINT32_MAX};
    if (ioctl(q->control, CREATE_ENDPOINT, &info)) {
        rc = -errno;
        close(q->control);
        q->control = -1;
        return rc;
    }
    for (unsigned i = 0; i < 20; i++) {
        rc = find_port(parent, "DATA5_CNTL", q->endpoint, sizeof(q->endpoint));
        if (!rc) {
            q->fd = open_character(q->endpoint);
            if (q->fd >= 0)
                return 0;
            rc = q->fd;
        }
        usleep(50000);
    }
    fprintf(stderr, "RIL: owned RPMSG endpoint failed to open (%d), metadata path %s\n", rc,
            q->endpoint);
    close(q->control);
    q->control = -1;
    return rc;
}
static int result_error(const uint8_t *body, size_t bytes, int result) {
    if (!result)
        return 0;
    size_t n;
    const uint8_t *v = qmux_tlv(body, bytes, 2, &n);
    return v && n == 4 && qmux_read16(v + 2) ? qmux_read16(v + 2) : -EBADMSG;
}
static int dispatch_frame(struct qmux *q, const uint8_t *frame, size_t size) {
    uint8_t service, client;
    uint16_t message;
    const uint8_t *body;
    size_t bytes;
    int rc = qmux_indication(frame, size, &service, &client, &message, &body, &bytes);
    if (rc < 0)
        return rc;
    if (rc > 0) {
        if (q->indication && q->clients[service] &&
            (client == q->clients[service] || client == 255))
            q->indication(service, message, body, bytes);
        return 1;
    }
    if (q->async_callback) {
        rc = qmux_response(frame, size, q->async_service, q->async_client, q->async_transaction,
                           q->async_message, &body, &bytes);
        if (rc == -1)
            return -EBADMSG;
        if (rc != -2) {
            qmux_async_callback callback = q->async_callback;
            q->async_callback = NULL;
            callback(result_error(body, bytes, rc), body, bytes);
            return 1;
        }
    }
    return 0;
}
int qmux_poll(struct qmux *q) {
    if (q->fd < 0)
        return -ENODEV;
    for (unsigned i = 0; i < 16; i++) {
        struct pollfd p = {.fd = q->fd, .events = POLLIN};
        int rc = poll(&p, 1, 0);
        if (rc < 0)
            return errno == EINTR ? 0 : -errno;
        if (!rc)
            return 0;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -ENETRESET;
        ssize_t n = read(q->fd, q->response, sizeof(q->response));
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        if (n <= 0)
            return -EIO;
        if (dispatch_frame(q, q->response, n) < 0)
            return -EBADMSG;
    }
    return 0;
}
static uint16_t next_transaction(struct qmux *q) {
    q->transaction = (q->transaction % 255) + 1;
    if (q->async_callback && q->transaction == q->async_transaction)
        q->transaction = (q->transaction % 255) + 1;
    return q->transaction;
}
static int write_request(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                         const void *data, size_t size, uint16_t transaction) {
    if (q->fd < 0)
        return -ENODEV;
    if (size > 1024 || (!data && size))
        return -EINVAL;
    uint8_t request[1037];
    size_t head = service ? 13 : 12;
    memset(request, 0, head);
    request[0] = 1;
    qmux_write16(request + 1, head + size - 1);
    request[4] = service;
    request[5] = client;
    if (service)
        qmux_write16(request + 7, transaction);
    else
        request[7] = transaction;
    qmux_write16(request + (service ? 9 : 8), message);
    qmux_write16(request + (service ? 11 : 10), size);
    if (size)
        memcpy(request + head, data, size);
    if (write(q->fd, request, head + size) != (ssize_t)(head + size))
        return -EIO;
    return 0;
}
int qmux_send_async(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                    const void *data, size_t size, qmux_async_callback callback) {
    if (!callback || !service || !client || q->clients[service] != client)
        return -EINVAL;
    if (q->async_callback)
        return -EBUSY;
    uint16_t transaction = next_transaction(q);
    q->async_service = service;
    q->async_client = client;
    q->async_transaction = transaction;
    q->async_message = message;
    q->async_callback = callback;
    int rc = write_request(q, service, client, message, data, size, transaction);
    if (rc)
        q->async_callback = NULL;
    return rc;
}
static int exchange(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                    const void *data, size_t size, const uint8_t **response, size_t *bytes,
                    int timeout) {
    uint16_t transaction = next_transaction(q);
    int send_rc = write_request(q, service, client, message, data, size, transaction);
    if (send_rc)
        return send_rc;
    int64_t begin = milliseconds();
    if (begin < 0)
        return -EIO;
    int64_t end = begin + timeout;
    while (milliseconds() < end) {
        int64_t now = milliseconds();
        if (now < 0)
            return -EIO;
        int64_t left = end - now;
        if (left <= 0)
            break;
        struct pollfd p = {.fd = q->fd, .events = POLLIN};
        int rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return -errno;
        if (!rc)
            break;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -ENETRESET;
        ssize_t n = read(q->fd, q->response, sizeof(q->response));
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            return -EIO;
        rc = dispatch_frame(q, q->response, n);
        if (rc < 0)
            return -EBADMSG;
        if (rc > 0)
            continue;
        rc = qmux_response(q->response, n, service, client, transaction, message, response, bytes);
        if (rc == -2)
            continue;
        if (rc < 0) {
            __android_log_print(ANDROID_LOG_WARN, "SupermanRIL",
                                "Rejected QMUX frame n=%zd header=%02x %02x%02x %02x %02x %02x "
                                "%02x %02x%02x %02x%02x %02x%02x",
                                n, q->response[0], q->response[1], q->response[2], q->response[3],
                                q->response[4], q->response[5], q->response[6], q->response[7],
                                q->response[8], q->response[9], q->response[10], q->response[11],
                                q->response[12]);
            return -EBADMSG;
        }
        if (!rc)
            return 0;
        size_t len = 0;
        const uint8_t *result = qmux_tlv(*response, *bytes, 2, &len);
        return result && len == 4 && qmux_read16(result + 2) ? qmux_read16(result + 2) : -EBADMSG;
    }
    return -ETIMEDOUT;
}
int qmux_request(struct qmux *q, uint8_t service, uint8_t client, uint16_t message,
                 const void *data, size_t size, const uint8_t **response, size_t *bytes) {
    return exchange(q, service, client, message, data, size, response, bytes, 3000);
}
int qmux_client(struct qmux *q, uint8_t service) {
    if (!service)
        return 0;
    if (q->clients[service])
        return q->clients[service];
    uint8_t request[] = {1, 1, 0, service};
    const uint8_t *body;
    size_t bytes, len;
    int rc = qmux_request(q, 0, 0, 0x22, request, sizeof(request), &body, &bytes);
    if (rc)
        return -EIO;
    const uint8_t *v = qmux_tlv(body, bytes, 1, &len);
    if (!v || len != 2 || v[0] != service || !v[1])
        return -EBADMSG;
    q->clients[service] = v[1];
    return v[1];
}
int qmux_release_client(struct qmux *q, uint8_t service) {
    if (!service || !q->clients[service])
        return -EINVAL;
    uint8_t client = q->clients[service], args[] = {1, 2, 0, service, client};
    const uint8_t *body;
    size_t bytes;
    int rc = exchange(q, 0, 0, 0x23, args, sizeof(args), &body, &bytes, 3000);
    if (!rc) {
        q->clients[service] = 0;
        if (q->async_callback && q->async_service == service && q->async_client == client)
            q->async_callback = NULL;
    }
    return rc;
}
void qmux_close(struct qmux *q) {
    q->async_callback = NULL;q->indication=NULL;
    if (q->fd >= 0) {
        for (unsigned s = 1; s < 256; s++)
            if (q->clients[s]) {
                uint8_t request[] = {1, 2, 0, s, q->clients[s]};
                const uint8_t *v;
                size_t bytes;
                (void)exchange(q, 0, 0, 0x23, request, sizeof(request), &v, &bytes, 300);
                q->clients[s] = 0;
            }
        (void)ioctl(q->fd, DESTROY_ENDPOINT);
        close(q->fd);
        q->fd = -1;
    }
    if (q->control >= 0) {
        close(q->control);
        q->control = -1;
    }
}

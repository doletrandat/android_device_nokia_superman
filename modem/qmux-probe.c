// SPDX-License-Identifier: Apache-2.0
/* Fixed QMI status queries; explicit temporary CFUN test restores radio-off. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct endpoint_info {
    char name[32];
    uint32_t src, dst;
};
#define CREATE_EPT _IOW(0xb5, 1, struct endpoint_info)
#define DESTROY_EPT _IO(0xb5, 2)
#include "qmux-codec.h"
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int allow_sim2_session;
static size_t build(uint8_t *p, uint8_t svc, uint8_t cid, uint16_t txn, uint16_t msg,
                    const uint8_t *args, size_t n) {
    /* No caller-selected network/SIM/NV mutation requests. */
    if ((svc == 0 && msg != 0x21 && msg != 0x22 && msg != 0x23) ||
        (svc == 2 && msg != 0x23 && msg != 0x2d) || (svc == 3 && msg != 0x24 && msg != 0x45) ||
        (svc == 11 && msg != 0x2f && msg != 0x38) ||
        (svc != 0 && svc != 2 && svc != 3 && svc != 11) || n > 42 || (!svc && txn > 255))
        return 0;
    if (svc == 3 && msg == 0x45 &&
        (n != 4 || !args || args[0] != 1 || args[1] != 1 || args[2] != 0 || args[3] > 1))
        return 0;
    if (svc == 11 && msg == 0x38) {
        if (!allow_sim2_session || !args || n < 5 || args[0] != 1 || args[1] != 2 || args[2] != 0 ||
            args[3] != 2 || args[4] > 1)
            return 0;
        if (n < 10 || args[5] != 0x10 || args[7] != 0 || args[8] != 2 || args[9] > 32 ||
            n != 10 + (size_t)args[9] || args[6] != 2 + args[9])
            return 0;
    }
    size_t h = svc ? 13 : 12;
    memset(p, 0, h + n);
    p[0] = 1;
    qmux_write16(p + 1, h + n - 1);
    p[4] = svc;
    p[5] = cid;
    if (svc)
        qmux_write16(p + 7, txn);
    else
        p[7] = txn;
    qmux_write16(p + (svc ? 9 : 8), msg);
    qmux_write16(p + (svc ? 11 : 10), n);
    if (n)
        memcpy(p + h, args, n);
    return h + n;
}
static uint8_t received[4096];
static uint16_t transaction;
static int query(int fd, uint8_t svc, uint8_t cid, uint16_t msg, const uint8_t *args, size_t n,
                 const uint8_t **out, size_t *len) {
    uint8_t request[64];
    uint16_t txn = ++transaction;
    size_t size = build(request, svc, cid, txn, msg, args, n);
    if (!size || write(fd, request, size) != (ssize_t)size)
        return -1;
    int64_t end = now_ms() + 3000;
    while (now_ms() < end) {
        int64_t left = end - now_ms();
        if (left <= 0)
            break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            break;
        ssize_t count = read(fd, received, sizeof(received));
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return -1;
        rc = qmux_response(received, count, svc, cid, txn, msg, out, len);
        if (rc == -2)
            continue;
        size_t result_len = 0;
        const uint8_t *result = rc >= 0 ? qmux_tlv(*out, *len, 2, &result_len) : NULL;
        printf("{\"service\":%u,\"message\":%u,\"result\":%d,\"qmi_error\":%d}\n", svc, msg, rc,
               result && result_len == 4 ? qmux_read16(result + 2) : -1);
        return rc;
    }
    fprintf(stderr, "QMI timeout service=%u message=%u\n", svc, msg);
    return -1;
}
/* Discover only the main-modem control device; never accept arbitrary device paths. */
static int find_device(const char *parent, const char *name, char *dev, size_t size) {
    DIR *dir = opendir("/sys/class/rpmsg");
    if (!dir)
        return -1;
    struct dirent *entry;
    int matches = 0;
    char path[PATH_MAX], resolved[PATH_MAX], value[64];
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/rpmsg/%s", entry->d_name);
        if (!realpath(path, resolved) || !strstr(resolved, parent))
            continue;
        if (name) {
            snprintf(path, sizeof(path), "/sys/class/rpmsg/%s/name", entry->d_name);
            FILE *f = fopen(path, "re");
            if (!f)
                continue;
            char *got = fgets(value, sizeof(value), f);
            fclose(f);
            if (!got)
                continue;
            value[strcspn(value, "\n")] = 0;
            if (strcmp(value, name))
                continue;
        } else if (strncmp(entry->d_name, "rpmsg_ctrl", 10))
            continue;
        if (snprintf(dev, size, "/dev/%s", entry->d_name) >= (int)size) {
            closedir(dir);
            return -1;
        }
        matches++;
    }
    closedir(dir);
    return matches == 1 ? 0 : matches ? -EEXIST : -ENOENT;
}
static int open_char(const char *path) {
    struct stat st;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}
static char at_response[4096];
static int at_terminal(const char *response) {
    if (strstr(response, "\r\nOK\r\n"))
        return 0;
    if (strstr(response, "\r\nERROR\r\n") || strstr(response, "\r\n+CME ERROR:"))
        return 1;
    return -1;
}
static int at_command(int fd, const char *command, int timeout_ms) {
    size_t n = strlen(command), used = 0;
    at_response[0] = 0;
    if (write(fd, command, n) != (ssize_t)n)
        return -1;
    int64_t end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int64_t left = end - now_ms();
        if (left <= 0)
            break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        ssize_t count = read(fd, at_response + used, sizeof(at_response) - used - 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return -1;
        used += (size_t)count;
        at_response[used] = 0;
        r = at_terminal(at_response);
        if (r >= 0) {
            for (size_t i = 0; i < used; i++)
                putchar((at_response[i] >= 32 && at_response[i] < 127) || at_response[i] == '\r' ||
                                at_response[i] == '\n'
                            ? at_response[i]
                            : '?');
            return r;
        }
        if (used == sizeof(at_response) - 1)
            return -1;
    }
    fprintf(stderr, "AT command response deadline expired\n");
    return -1;
}
static int cmee_mode(const char *response) {
    const char *line = strstr(response, "+CMEE: ");
    if (!line || line[7] < '0' || line[7] > '2' || (line[8] != '\r' && line[8] != '\n'))
        return -1;
    return line[7] - '0';
}
static int at_power_test(int fd, int ril_check, int hold_seconds) {
    if (at_command(fd, "AT+CFUN?\r", 3000) || !strstr(at_response, "+CFUN: 0")) {
        fprintf(stderr, "Radio test requires original CFUN=0\n");
        return 1;
    }
    if (at_command(fd, "AT+CMEE?\r", 3000))
        return 1;
    int old_cmee = cmee_mode(at_response);
    if (old_cmee < 0)
        return 1;
    int rc = at_command(fd, "AT+CFUN=1\r", 15000);
    if (!rc) {
        printf("CFUN=1 acknowledged; holding for %d seconds before SIM/network queries\n",
               hold_seconds);
        fflush(stdout);
        struct timespec delay = {.tv_sec = hold_seconds};
        while (nanosleep(&delay, &delay) && errno == EINTR) {
        }
        const char *commands[] = {"AT+CMEE=1\r", "AT+CFUN?\r", "AT+CPIN?\r", "AT+CREG?\r",
                                  "AT+CGMR\r"};
        for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            if ((ril_check || i == 1) && at_command(fd, commands[i], 3000))
                rc = 1;
    }
    const char *restore_cmee[] = {"AT+CMEE=0\r", "AT+CMEE=1\r", "AT+CMEE=2\r"};
    if (ril_check && at_command(fd, restore_cmee[old_cmee], 3000))
        rc = 1;
    /* The physical preflight proved CFUN=0; restore it on every attempt. */
    if (at_command(fd, "AT+CFUN=0\r", 15000))
        rc = 1;
    if (at_command(fd, "AT+CFUN?\r", 3000) || !strstr(at_response, "+CFUN: 0"))
        rc = 1;
    else
        puts("Original CFUN=0 restored");
    return rc;
}
static int at_check(int fd) {
    static const char *const commands[] = {"AT\r", "AT+CPIN?\r", "AT+CFUN?\r", "AT+CREG?\r"};
    int replies = 0;
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        size_t n = strlen(commands[i]);
        if (write(fd, commands[i], n) != (ssize_t)n)
            return 1;
        printf("AT query %u response:\n", i);
        int64_t end = now_ms() + 2000;
        while (now_ms() < end) {
            int64_t left = end - now_ms();
            if (left <= 0)
                break;
            struct pollfd p = {.fd = fd, .events = POLLIN};
            int r = poll(&p, 1, (int)left);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            uint8_t data[1024];
            ssize_t len = read(fd, data, sizeof(data));
            if (len < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (len <= 0)
                return 1;
            replies++;
            for (ssize_t j = 0; j < len; j++)
                putchar((data[j] >= 32 && data[j] < 127) || data[j] == '\r' || data[j] == '\n'
                            ? data[j]
                            : '?');
            putchar('\n');
        }
    }
    return replies ? 0 : 1;
}
static int card_status(const uint8_t *data, size_t len, int print) {
    if (len < 9)
        return -1;
    if (print)
        printf("UIM GW provisioning indexes: primary=%04x secondary=%04x\n", qmux_read16(data),
               qmux_read16(data + 4));
    size_t off = 9;
    for (unsigned slot = 0; slot < data[8]; slot++) {
        if (len - off < 6)
            return -1;
        unsigned state = data[off], error = data[off + 4], apps = data[off + 5];
        off += 6;
        for (unsigned app = 0; app < apps; app++) {
            if (len - off < 7)
                return -1;
            unsigned app_type = data[off], app_state = data[off + 1];
            unsigned perso_state = data[off + 2], perso_feature = data[off + 3],
                     perso_retries = data[off + 4];
            unsigned aid_len = data[off + 6];
            off += 7;
            if (len - off < (size_t)aid_len + 7)
                return -1;
            if (print)
                printf("UIM slot %u application %u: type=%u state=%u PIN1=%u PIN2=%u "
                       "personalization=%u feature=%u retries=%u\n",
                       slot, app, app_type, app_state, data[off + aid_len + 1],
                       data[off + aid_len + 4], perso_state, perso_feature, perso_retries);
            off += aid_len + 7;
        }
        if (print)
            printf("UIM slot %u: state=%u error=%u applications=%u\n", slot, state, error, apps);
    }
    return off == len ? 0 : -1;
}
static int sim2_session(int fd, uint8_t cid, const uint8_t *data, size_t len, int activate) {
    if (card_status(data, len, 0) || data[8] < 2)
        return -1;
    uint16_t previous = qmux_read16(data + 4);
    if ((activate && previous != 0xffff) || (!activate && previous != 0x0100))
        return -1;
    const uint8_t *aid = NULL;
    unsigned aid_len = 0;
    size_t off = 9;
    for (unsigned slot = 0; slot < data[8]; slot++) {
        unsigned state = data[off], apps = data[off + 5];
        off += 6;
        if (slot == 1 && (state != 1 || apps != 1))
            return -1;
        for (unsigned app = 0; app < apps; app++) {
            unsigned n = data[off + 6];
            if (slot == 1 && app == 0) {
                if ((data[off] != 1 && data[off] != 2) || n > 32)
                    return -1;
                aid = data + off + 7;
                aid_len = n;
            }
            off += 14 + n;
        }
    }
    if (!aid)
        return -1;
    uint8_t args[42] = {1, 2, 0, 2, activate, 0x10, 2 + aid_len, 0, 2, aid_len};
    if (aid_len)
        memcpy(args + 10, aid, aid_len);
    const uint8_t *v;
    size_t bytes;
    int rc = query(fd, 11, cid, 0x38, args, 10 + aid_len, &v, &bytes);
    if (rc)
        return rc;
    puts(activate ? "SIM2 secondary GSM session activation accepted"
                  : "Owned SIM2 secondary GSM session deactivation accepted");
    if (query(fd, 11, cid, 0x2f, NULL, 0, &v, &bytes))
        return -1;
    data = qmux_tlv(v, bytes, 0x10, &len);
    return data ? card_status(data, len, 1) : -1;
}
static int test(void) {
    assert(cmee_mode("\r\n+CMEE: 2\r\nOK\r\n") == 2);
    assert(cmee_mode("+CMEE: 0\r") == 0);
    assert(cmee_mode("+CMEE: 22\r") < 0);
    assert(cmee_mode("+CMEE: ") < 0);
    assert(at_terminal("\r\nOK\r\n") == 0);
    assert(at_terminal("AT+CFUN?\r\n+CFUN: 0\r\nOK\r\n") == 0);
    assert(at_terminal("\r\nERROR\r\n") == 1);
    assert(at_terminal("\r\n+CME ERROR: 10\r\n") == 1);
    assert(at_terminal("\r\nO") == -1);
    uint8_t p[40] = {1, 18, 0, 0x80, 0, 0, 1, 1, 0x21, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    const uint8_t *v = NULL;
    size_t n = 0;
    assert(qmux_response(p, 19, 0, 0, 1, 0x21, &v, &n) == 0 && n == 7);
    for (size_t i = 0; i < 19; i++)
        assert(qmux_response(p, i, 0, 0, 1, 0x21, &v, &n) < 0);
    assert(qmux_response(p, 19, 0, 0, 2, 0x21, &v, &n) == -2);
    p[13] = 5;
    assert(qmux_response(p, 19, 0, 0, 1, 0x21, &v, &n) < 0);
    p[13] = 4;
    p[15] = 1;
    assert(qmux_response(p, 19, 0, 0, 1, 0x21, &v, &n) == 1);
    p[15] = 0;
    assert(build(p, 0, 0, 1, 0x21, NULL, 0) == 12 && qmux_read16(p + 1) == 11);
    assert(build(p, 2, 3, 2, 0x23, NULL, 0) == 13 && p[4] == 2 && p[5] == 3);
    assert(!build(p, 2, 3, 2, 0x2e, NULL, 0));
    assert(build(p, 3, 3, 2, 0x24, NULL, 0) == 13);
    assert(!build(p, 3, 3, 2, 0x23, NULL, 0));
    uint8_t bind[] = {1, 1, 0, 1};
    assert(build(p, 3, 3, 2, 0x45, bind, sizeof(bind)) == 17);
    bind[3] = 2;
    assert(!build(p, 3, 3, 2, 0x45, bind, sizeof(bind)));
    assert(!build(p, 3, 3, 2, 0x45, NULL, 0));
    assert(!build(p, 11, 3, 2, 0x26, NULL, 0));
    assert(!build(p, 0, 0, 256, 0x21, NULL, 0));
    uint8_t session[] = {1, 2, 0, 2, 1, 0x10, 2, 0, 2, 0};
    assert(!build(p, 11, 3, 2, 0x38, session, sizeof(session)));
    allow_sim2_session = 1;
    assert(build(p, 11, 3, 2, 0x38, session, sizeof(session)) == 23);
    session[8] = 1;
    assert(!build(p, 11, 3, 2, 0x38, session, sizeof(session)));
    session[8] = 2;
    session[9] = 32;
    assert(!build(p, 11, 3, 2, 0x38, session, sizeof(session)));
    session[9] = 0;
    session[4] = 0;
    assert(build(p, 11, 3, 2, 0x38, session, sizeof(session)) == 23);
    assert(!build(p, 11, 3, 2, 0x38, session, 5));
    allow_sim2_session = 0;
    uint8_t dup[] = {1, 1, 0, 42, 1, 1, 0, 43};
    assert(!qmux_tlv(dup, sizeof(dup), 1, &n));
    uint8_t absent[21] = {0};
    absent[8] = 2;
    assert(card_status(absent, sizeof(absent), 0) == 0);
    for (size_t n = 0; n < sizeof(absent); n++)
        assert(card_status(absent, n, 0) < 0);
    absent[14] = 1;
    assert(card_status(absent, sizeof(absent), 0) < 0);
    uint8_t present[29] = {0};
    present[8] = 1;
    present[9] = 1;
    present[14] = 1;
    assert(card_status(present, sizeof(present), 0) == 0);
    for (size_t n = 0; n < sizeof(present); n++)
        assert(card_status(present, n, 0) < 0);
    present[21] = 255;
    assert(card_status(present, sizeof(present), 0) < 0);
    puts(
        "PASS QMUX framing, transactions, TLV/card bounds, result codes and fixed query allowlist");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test"))
        return test();
    int at = 0, session_change = 0;
    if (argc == 2 && !strcmp(argv[1], "--at-check"))
        at = 1;
    else if (argc == 2 && !strcmp(argv[1], "--at-check-data4"))
        at = 2;
    else if (argc == 2 && !strcmp(argv[1], "--radio-power-test"))
        at = 3;
    else if (argc == 2 && !strcmp(argv[1], "--radio-status-test"))
        at = 4;
    else if (argc == 2 && !strcmp(argv[1], "--activate-sim2"))
        session_change = 1;
    else if (argc == 2 && !strcmp(argv[1], "--deactivate-sim2"))
        session_change = 2;
    else if (argc == 2 && !strcmp(argv[1], "--radio-registration-test"))
        at = 5;
    else if (argc != 1)
        return 2;
    allow_sim2_session = session_change != 0;
    const char *channel = at == 2 ? "DATA4" : at ? "DATA1" : "DATA5_CNTL";
    char ctrl[96], dev[96], parent[PATH_MAX], ctrlsys[128];
    if (find_device("/remoteproc2:smd-edge.rpmsg_ctrl.0.0/", NULL, ctrl, sizeof(ctrl))) {
        fprintf(stderr, "Exact modem control device missing\n");
        return 1;
    }
    snprintf(ctrlsys, sizeof(ctrlsys), "/sys/class/rpmsg/%s", strrchr(ctrl, '/') + 1);
    if (!realpath(ctrlsys, parent) || strlen(parent) + 1 >= sizeof(parent))
        return 1;
    strcat(parent, "/");
    if (find_device(parent, channel, dev, sizeof(dev)) != -ENOENT) {
        fprintf(stderr, "Existing QMI endpoint: refusing to adopt it\n");
        return 1;
    }
    int ctl = open_char(ctrl);
    if (ctl < 0) {
        perror("open control");
        return 1;
    }
    struct endpoint_info info = {.src = UINT32_MAX, .dst = UINT32_MAX};
    snprintf(info.name, sizeof(info.name), "%s", channel);
    if (ioctl(ctl, CREATE_EPT, &info)) {
        perror("create endpoint");
        close(ctl);
        return 1;
    }
    printf("Created owned %s endpoint under %s\n", channel, ctrl);
    fflush(stdout);
    int fd = -1;
    for (int i = 0; i < 20; i++) {
        if (!find_device(parent, channel, dev, sizeof(dev)) && (fd = open_char(dev)) >= 0)
            break;
        usleep(50000);
    }
    if (fd < 0) {
        perror("open owned endpoint; kernel cleanup required");
        close(ctl);
        return 1;
    }
    const uint8_t *v, *data;
    size_t bytes, len;
    int rc = 1;
    if (at) {
        rc = at >= 3 ? at_power_test(fd, at == 3, at == 5 ? 90 : 20) : at_check(fd);
        goto done;
    }
    if (query(fd, 0, 0, 0x21, NULL, 0, &v, &bytes))
        goto done;
    data = qmux_tlv(v, bytes, 1, &len);
    if (!data || len < 1 || len != 1 + (size_t)data[0] * 5)
        goto done;
    int supported[3] = {0};
    const uint8_t services[] = {2, 3, 11};
    for (unsigned i = 0; i < data[0]; i++) {
        const uint8_t *e = data + 1 + i * 5;
        printf("{\"supported_service\":%u,\"major\":%u,\"minor\":%u}\n", e[0], qmux_read16(e + 1),
               qmux_read16(e + 3));
        for (unsigned j = 0; j < 3; j++)
            if (e[0] == services[j])
                supported[j] = 1;
    }
    rc = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t svc = services[i];
        if (!supported[i])
            continue;
        uint8_t alloc[] = {1, 1, 0, svc};
        if (query(fd, 0, 0, 0x22, alloc, sizeof(alloc), &v, &bytes)) {
            rc = 1;
            continue;
        }
        data = qmux_tlv(v, bytes, 1, &len);
        if (!data || len != 2 || data[0] != svc || !data[1]) {
            rc = 1;
            continue;
        }
        uint8_t cid = data[1];
        if (svc == 3) {
            for (unsigned sub = 0; sub < 2; sub++) {
                uint8_t bind[] = {1, 1, 0, sub};
                printf("NAS client subscription %u\n", sub);
                if (query(fd, svc, cid, 0x45, bind, sizeof(bind), &v, &bytes)) {
                    rc = 1;
                    continue;
                }
                if (query(fd, svc, cid, 0x24, NULL, 0, &v, &bytes)) {
                    rc = 1;
                    continue;
                }
                data = qmux_tlv(v, bytes, 1, &len);
                if (!data || len < 5 || len != 5 + (size_t)data[4])
                    rc = 1;
                else
                    printf("NAS subscription %u registration=%u CS=%u PS=%u network=%u "
                           "interfaces=%u\n",
                           sub, data[0], data[1], data[2], data[3], data[4]);
            }
        } else {
            int r = query(fd, svc, cid, svc == 11 ? 0x2f : 0x23, NULL, 0, &v, &bytes);
            if (r)
                rc = 1;
            else {
                data = qmux_tlv(v, bytes, svc == 11 ? 0x10 : 1, &len);
                if (!data)
                    rc = 1;
                else if (svc == 11) {
                    if (card_status(data, len, 1))
                        rc = 1;
                    else if (session_change &&
                             sim2_session(fd, cid, data, len, session_change == 1))
                        rc = 1;
                } else {
                    printf("Firmware revision: ");
                    for (size_t j = 0; j < len && j < 256; j++)
                        putchar(data[j] >= 32 && data[j] < 127 ? data[j] : '?');
                    putchar('\n');
                }
            }
        }
        if (svc == 2 && query(fd, svc, cid, 0x2d, NULL, 0, &v, &bytes) == 0) {
            data = qmux_tlv(v, bytes, 1, &len);
            if (data && len == 1)
                printf("DMS operating mode: %u\n", data[0]);
            data = qmux_tlv(v, bytes, 0x10, &len);
            if (data && len == 2)
                printf("DMS offline reason: %u\n", qmux_read16(data));
            data = qmux_tlv(v, bytes, 0x11, &len);
            if (data && len == 1)
                printf("DMS hardware restricted: %u\n", data[0]);
        }
        uint8_t release[] = {1, 2, 0, svc, cid};
        if (query(fd, 0, 0, 0x23, release, sizeof(release), &v, &bytes))
            rc = 1;
    }
done:
    if (ioctl(fd, DESTROY_EPT)) {
        perror("destroy owned endpoint");
        rc = 1;
    } else
        puts("Owned endpoint destroyed");
    close(fd);
    close(ctl);
    return rc;
}

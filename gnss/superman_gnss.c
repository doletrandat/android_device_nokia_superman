// SPDX-License-Identifier: Apache-2.0
/*
 * Lumia 730 (superman) legacy GPS HAL: QMI LOC client over QRTR.
 *
 * The MPSS firmware advertises QMI LOC (service 16, version 2) on QRTR node 0.
 * This is independent of the DATA5_CNTL QMUX channel owned by the Radio HAL.
 * The AOSP android.hardware.gnss@1.0-impl wrapper loads this module.
 *
 * Threading: one reader thread (created through create_thread_cb, as gps.h
 * requires for callbacks) owns all incoming traffic. Requests from binder
 * threads only send; responses are checked and logged by the reader.
 */
#define _GNU_SOURCE
#define LOG_TAG "superman_gnss"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <hardware/gps.h>
#include <linux/qrtr.h>
#include <log/log.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define LOC_SERVICE 16
#define LOC_VERSION 2

#define QMI_REQUEST 0
#define QMI_RESPONSE 2
#define QMI_INDICATION 4

#define LOC_REG_EVENTS 0x0021
#define LOC_START 0x0022
#define LOC_STOP 0x0023
#define LOC_IND_POSITION 0x0024
#define LOC_IND_SV_INFO 0x0025
#define LOC_IND_NMEA 0x0026
#define LOC_IND_ENGINE_STATE 0x002B
#define LOC_IND_FIX_SESSION 0x002C
#define LOC_INJECT_UTC 0x0038
#define LOC_INJECT_POSITION 0x0039
#define LOC_DELETE_ASSIST 0x0044
#define LOC_SET_OP_MODE 0x004A

#define EV_POSITION (1u << 0)
#define EV_SV_INFO (1u << 1)
#define EV_NMEA (1u << 2)
#define EV_ENGINE_STATE (1u << 7)
#define EV_FIX_SESSION (1u << 8)

#define LOC_SESSION_ID 1
#define LOC_OP_MODE_STANDALONE 4
#define LOC_RECUR_PERIODIC 1
#define LOC_RECUR_SINGLE 2
#define LOC_INTERMEDIATE_DISABLE 2

#define LOC_SYS_GPS 1
#define LOC_SYS_GALILEO 2
#define LOC_SYS_SBAS 3
#define LOC_SYS_COMPASS 4
#define LOC_SYS_GLONASS 5

#define SV_VALID_SYSTEM (1u << 0)
#define SV_VALID_ID (1u << 1)
#define SV_VALID_PROCESS (1u << 3)
#define SV_VALID_INFO_MASK (1u << 4)
#define SV_VALID_ELEVATION (1u << 5)
#define SV_VALID_AZIMUTH (1u << 6)
#define SV_VALID_SNR (1u << 7)

static struct {
    pthread_mutex_t lock; /* fd, peer, txn and session fields */
    int fd;
    int wake[2]; /* reader shutdown pipe */
    struct sockaddr_qrtr peer;
    int have_peer;
    uint16_t txn;
    int session_wanted;
    uint32_t interval_ms;
    int single_shot;
    GpsCallbacks cb;
    pthread_t reader;
    int reader_running;
    /* QMI LOC SV ids used in the most recent fix (Qualcomm numbering). */
    uint16_t used[64];
    int used_count;
    /* Last time injected by the framework, kept for a modem that comes up later. */
    int64_t time_utc, time_boot_ms;
    uint32_t time_unc;
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .wake = {-1, -1}, .interval_ms = 1000};

static uint16_t rd16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static float rdf(const uint8_t *p) {
    uint32_t v = rd32(p);
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}
static double rdd(const uint8_t *p) {
    uint64_t v = rd64(p);
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

/* TLV buffer builder. */
struct tlvbuf {
    uint8_t data[512];
    size_t len;
    int overflow;
};
static void tlv_put(struct tlvbuf *b, uint8_t type, const void *value, size_t len) {
    if (b->len + 3 + len > sizeof(b->data)) {
        b->overflow = 1;
        return;
    }
    b->data[b->len++] = type;
    b->data[b->len++] = len & 0xff;
    b->data[b->len++] = len >> 8;
    memcpy(b->data + b->len, value, len);
    b->len += len;
}
static void tlv_u8(struct tlvbuf *b, uint8_t t, uint8_t v) { tlv_put(b, t, &v, 1); }
static void tlv_u32(struct tlvbuf *b, uint8_t t, uint32_t v) {
    uint8_t x[4] = {v, v >> 8, v >> 16, v >> 24};
    tlv_put(b, t, x, 4);
}
static void tlv_u64(struct tlvbuf *b, uint8_t t, uint64_t v) {
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = v >> (8 * i);
    tlv_put(b, t, x, 8);
}
static void tlv_f32(struct tlvbuf *b, uint8_t t, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    tlv_u32(b, t, u);
}
static void tlv_f64(struct tlvbuf *b, uint8_t t, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    tlv_u64(b, t, u);
}

/* Find TLV `type` in a QMI body. Returns pointer and length, or NULL. */
static const uint8_t *tlv_find(const uint8_t *body, size_t n, uint8_t type, size_t *len) {
    size_t off = 0;
    while (n - off >= 3) {
        uint8_t t = body[off];
        size_t l = rd16(body + off + 1);
        off += 3;
        if (l > n - off) return NULL;
        if (t == type) {
            *len = l;
            return body + off;
        }
        off += l;
    }
    return NULL;
}

/* Caller holds g.lock. */
static int send_locked(uint16_t msg, const struct tlvbuf *b) {
    if (g.fd < 0 || !g.have_peer) return -ENOTCONN;
    if (b && b->overflow) return -E2BIG;
    uint8_t pkt[7 + sizeof(b->data)];
    size_t len = b ? b->len : 0;
    uint16_t txn = ++g.txn ? g.txn : ++g.txn;
    pkt[0] = QMI_REQUEST;
    pkt[1] = txn & 0xff;
    pkt[2] = txn >> 8;
    pkt[3] = msg & 0xff;
    pkt[4] = msg >> 8;
    pkt[5] = len & 0xff;
    pkt[6] = len >> 8;
    if (len) memcpy(pkt + 7, b->data, len);
    ssize_t r = sendto(g.fd, pkt, 7 + len, 0, (struct sockaddr *)&g.peer, sizeof(g.peer));
    if (r != (ssize_t)(7 + len)) {
        int e = r < 0 ? errno : EIO;
        ALOGE("send msg 0x%04x failed: %s", msg, strerror(e));
        return -e;
    }
    ALOGV("sent msg 0x%04x txn %u len %zu", msg, txn, len);
    return 0;
}
static int send_req(uint16_t msg, const struct tlvbuf *b) {
    pthread_mutex_lock(&g.lock);
    int r = send_locked(msg, b);
    pthread_mutex_unlock(&g.lock);
    return r;
}

/* Caller holds g.lock. */
static void send_start_locked(void) {
    struct tlvbuf b = {0};
    tlv_u32(&b, 0x01, LOC_OP_MODE_STANDALONE);
    send_locked(LOC_SET_OP_MODE, &b);
    memset(&b, 0, sizeof(b));
    tlv_u8(&b, 0x01, LOC_SESSION_ID);
    tlv_u32(&b, 0x10, g.single_shot ? LOC_RECUR_SINGLE : LOC_RECUR_PERIODIC);
    tlv_u32(&b, 0x12, LOC_INTERMEDIATE_DISABLE);
    tlv_u32(&b, 0x13, g.interval_ms);
    send_locked(LOC_START, &b);
}
static void send_register_locked(void) {
    struct tlvbuf b = {0};
    tlv_u64(&b, 0x01, EV_POSITION | EV_SV_INFO | EV_NMEA | EV_ENGINE_STATE | EV_FIX_SESSION);
    send_locked(LOC_REG_EVENTS, &b);
}

static int64_t boot_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Caller holds g.lock. Sends the cached time aged by elapsed boot time. */
static int send_time_locked(void) {
    if (!g.time_utc) return 0;
    int64_t age = boot_ms() - g.time_boot_ms;
    if (age < 0) age = 0;
    struct tlvbuf b = {0};
    tlv_u64(&b, 0x01, (uint64_t)(g.time_utc + age));
    /* Uncertainty grows ~ with crystal drift; add 1 ms per 10 s of age. */
    tlv_u32(&b, 0x02, g.time_unc + (uint32_t)(age / 10000));
    return send_locked(LOC_INJECT_UTC, &b);
}

static void report_status(GpsStatusValue v) {
    if (!g.cb.status_cb) return;
    GpsStatus s = {.size = sizeof(s), .status = v};
    g.cb.status_cb(&s);
}

static int64_t now_utc_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void handle_position(const uint8_t *body, size_t n) {
    size_t l;
    const uint8_t *p;
    p = tlv_find(body, n, 0x01, &l);
    if (!p || l != 4) return;
    uint32_t status = rd32(p);
    /* Satellites used: list of u16 SV ids (u8 count prefix). */
    p = tlv_find(body, n, 0x2C, &l);
    g.used_count = 0;
    if (p && l >= 1) {
        size_t cnt = p[0];
        if (1 + 2 * cnt <= l) {
            for (size_t i = 0; i < cnt && i < 64; i++) g.used[g.used_count++] = rd16(p + 1 + 2 * i);
        }
    }
    if (status != 0) { /* not SUCCESS: in progress / no fix */
        ALOGV("position status %u", status);
        return;
    }
    const uint8_t *lat = tlv_find(body, n, 0x10, &l);
    if (!lat || l != 8) return;
    const uint8_t *lon = tlv_find(body, n, 0x11, &l);
    if (!lon || l != 8) return;
    GpsLocation loc = {.size = sizeof(loc)};
    loc.flags = GPS_LOCATION_HAS_LAT_LONG;
    loc.latitude = rdd(lat);
    loc.longitude = rdd(lon);
    if (!isfinite(loc.latitude) || !isfinite(loc.longitude) || fabs(loc.latitude) > 90 ||
        fabs(loc.longitude) > 180)
        return;
    if ((p = tlv_find(body, n, 0x12, &l)) && l == 4) {
        loc.accuracy = rdf(p);
        loc.flags |= GPS_LOCATION_HAS_HORIZONTAL_ACCURACY;
    }
    if ((p = tlv_find(body, n, 0x1A, &l)) && l == 4) {
        loc.altitude = rdf(p); /* WGS84 ellipsoid, as Android expects */
        loc.flags |= GPS_LOCATION_HAS_ALTITUDE;
    }
    if ((p = tlv_find(body, n, 0x18, &l)) && l == 4) {
        loc.speed = rdf(p);
        loc.flags |= GPS_LOCATION_HAS_SPEED;
    }
    if ((p = tlv_find(body, n, 0x20, &l)) && l == 4) {
        loc.bearing = rdf(p);
        loc.flags |= GPS_LOCATION_HAS_BEARING;
    }
    if ((p = tlv_find(body, n, 0x25, &l)) && l == 8)
        loc.timestamp = (GpsUtcTime)rd64(p);
    else
        loc.timestamp = now_utc_ms();
    ALOGD("fix acc=%.0fm sats=%d", loc.flags & GPS_LOCATION_HAS_HORIZONTAL_ACCURACY ? loc.accuracy : -1.0,
          g.used_count);
    if (g.cb.location_cb) g.cb.location_cb(&loc);
}

/* Qualcomm LOC SV id -> Android svid. */
static int map_sv(uint32_t sys, uint16_t id, GnssConstellationType *c) {
    switch (sys) {
        case LOC_SYS_GPS:
            if (id >= 193 && id <= 200) {
                *c = GNSS_CONSTELLATION_QZSS;
                return id;
            }
            *c = GNSS_CONSTELLATION_GPS;
            return id;
        case LOC_SYS_SBAS:
            *c = GNSS_CONSTELLATION_SBAS;
            return id < 120 ? id + 87 : id; /* 33..64 -> PRN 120..151 */
        case LOC_SYS_GLONASS:
            *c = GNSS_CONSTELLATION_GLONASS;
            return id >= 65 && id <= 96 ? id - 64 : id;
        case LOC_SYS_COMPASS:
            *c = GNSS_CONSTELLATION_BEIDOU;
            return id >= 201 && id <= 263 ? id - 200 : id;
        case LOC_SYS_GALILEO:
            *c = GNSS_CONSTELLATION_GALILEO;
            return id >= 301 && id <= 336 ? id - 300 : id;
        default:
            *c = GNSS_CONSTELLATION_UNKNOWN;
            return id;
    }
}

static void handle_sv_info(const uint8_t *body, size_t n) {
    size_t l;
    const uint8_t *p = tlv_find(body, n, 0x10, &l);
    if (!p || l < 1) return;
    size_t cnt = p[0];
    const size_t elem = 28;
    if (1 + cnt * elem > l) {
        ALOGW("sv info: %zu entries do not fit %zu bytes", cnt, l);
        return;
    }
    GnssSvStatus st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    for (size_t i = 0; i < cnt && st.num_svs < GNSS_MAX_SVS; i++) {
        const uint8_t *e = p + 1 + i * elem;
        uint32_t valid = rd32(e);
        uint32_t sys = rd32(e + 4);
        uint16_t id = rd16(e + 8);
        uint8_t navmask = e[15];
        if (!(valid & SV_VALID_SYSTEM) || !(valid & SV_VALID_ID) || !id) continue;
        GnssSvInfo *s = &st.gnss_sv_list[st.num_svs];
        s->size = sizeof(*s);
        s->svid = map_sv(sys, id, &s->constellation);
        if (s->constellation == GNSS_CONSTELLATION_UNKNOWN) continue;
        s->c_n0_dbhz = valid & SV_VALID_SNR ? rdf(e + 24) : 0;
        s->elevation = valid & SV_VALID_ELEVATION ? rdf(e + 16) : 0;
        s->azimuth = valid & SV_VALID_AZIMUTH ? rdf(e + 20) : 0;
        s->flags = 0;
        if (valid & SV_VALID_INFO_MASK) {
            if (navmask & 1) s->flags |= GNSS_SV_FLAGS_HAS_EPHEMERIS_DATA;
            if (navmask & 2) s->flags |= GNSS_SV_FLAGS_HAS_ALMANAC_DATA;
        }
        for (int k = 0; k < g.used_count; k++)
            if (g.used[k] == id) s->flags |= GNSS_SV_FLAGS_USED_IN_FIX;
        st.num_svs++;
    }
    if (g.cb.gnss_sv_status_cb) g.cb.gnss_sv_status_cb(&st);
}

static void handle_nmea(const uint8_t *body, size_t n) {
    size_t l;
    const uint8_t *p = tlv_find(body, n, 0x01, &l);
    if (!p || !l || !g.cb.nmea_cb) return;
    char s[256];
    size_t m = l < sizeof(s) - 1 ? l : sizeof(s) - 1;
    memcpy(s, p, m);
    s[m] = 0;
    m = strlen(s);
    while (m && (s[m - 1] == '\n' || s[m - 1] == '\r')) s[--m] = 0;
    if (m) g.cb.nmea_cb(now_utc_ms(), s, (int)m);
}

static void handle_qmi(const uint8_t *pkt, size_t n) {
    if (n < 7) return;
    uint8_t type = pkt[0];
    uint16_t msg = rd16(pkt + 3);
    size_t len = rd16(pkt + 5);
    if (len > n - 7) return;
    const uint8_t *body = pkt + 7;
    if (type == QMI_RESPONSE) {
        size_t l;
        const uint8_t *r = tlv_find(body, len, 0x02, &l);
        if (r && l == 4 && rd16(r))
            ALOGW("msg 0x%04x failed: qmi error %u", msg, rd16(r + 2));
        return;
    }
    if (type != QMI_INDICATION) return;
    switch (msg) {
        case LOC_IND_POSITION:
            handle_position(body, len);
            break;
        case LOC_IND_SV_INFO:
            handle_sv_info(body, len);
            break;
        case LOC_IND_NMEA:
            handle_nmea(body, len);
            break;
        case LOC_IND_ENGINE_STATE: {
            size_t l;
            const uint8_t *p = tlv_find(body, len, 0x01, &l);
            if (p && l == 4) {
                uint32_t s = rd32(p);
                ALOGI("engine %s", s == 1 ? "on" : s == 2 ? "off" : "?");
                if (s == 1) report_status(GPS_STATUS_ENGINE_ON);
                if (s == 2) report_status(GPS_STATUS_ENGINE_OFF);
            }
            break;
        }
        case LOC_IND_FIX_SESSION: { /* TLV 0x01: session state, 1 started / 2 finished */
            size_t l;
            const uint8_t *p = tlv_find(body, len, 0x01, &l);
            if (p && l == 4) ALOGI("fix session state %u", rd32(p));
            break;
        }
        default: {
            /* Async completion indications of set/inject messages carry status TLV 0x01. */
            size_t l;
            const uint8_t *p = tlv_find(body, len, 0x01, &l);
            if (p && l == 4 && rd32(p))
                ALOGW("indication 0x%04x status %u", msg, rd32(p));
            break;
        }
    }
}

static void handle_ctrl(const struct qrtr_ctrl_pkt *c) {
    uint32_t cmd = le32toh(c->cmd);
    if (cmd != QRTR_TYPE_NEW_SERVER && cmd != QRTR_TYPE_DEL_SERVER) return;
    uint32_t svc = le32toh(c->server.service);
    uint32_t inst = le32toh(c->server.instance);
    uint32_t node = le32toh(c->server.node), port = le32toh(c->server.port);
    if (svc != LOC_SERVICE || (inst & 0xff) != LOC_VERSION) return;
    pthread_mutex_lock(&g.lock);
    if (cmd == QRTR_TYPE_NEW_SERVER) {
        g.peer.sq_family = AF_QIPCRTR;
        g.peer.sq_node = node;
        g.peer.sq_port = port;
        g.have_peer = 1;
        ALOGI("LOC service at node %u port %u", node, port);
        send_register_locked();
        send_time_locked();
        if (g.session_wanted) send_start_locked();
    } else if (g.have_peer && g.peer.sq_node == node && g.peer.sq_port == port) {
        ALOGW("LOC service went away (modem stopped?)");
        g.have_peer = 0;
    }
    pthread_mutex_unlock(&g.lock);
}

static void reader_main(void *arg) {
    (void)arg;
    uint8_t buf[8192];
    for (;;) {
        struct pollfd p[2] = {{.fd = g.fd, .events = POLLIN}, {.fd = g.wake[0], .events = POLLIN}};
        int r = poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            ALOGE("poll: %s", strerror(errno));
            break;
        }
        if (p[1].revents) break;
        if (!(p[0].revents & POLLIN)) continue;
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g.fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == ENETRESET) continue; /* remote node reset; DEL_SERVER follows */
            ALOGE("recv: %s", strerror(errno));
            continue;
        }
        if (from.sq_port == QRTR_PORT_CTRL) {
            if ((size_t)n >= sizeof(struct qrtr_ctrl_pkt)) handle_ctrl((struct qrtr_ctrl_pkt *)buf);
            continue;
        }
        pthread_mutex_lock(&g.lock);
        int ours = g.have_peer && from.sq_node == g.peer.sq_node && from.sq_port == g.peer.sq_port;
        pthread_mutex_unlock(&g.lock);
        if (ours) handle_qmi(buf, (size_t)n);
    }
}

static int gps_init(GpsCallbacks *callbacks) {
    if (g.reader_running) return 0;
    memset(&g.cb, 0, sizeof(g.cb));
    memcpy(&g.cb, callbacks, callbacks->size < sizeof(g.cb) ? callbacks->size : sizeof(g.cb));
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ALOGE("qrtr socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_qrtr local;
    socklen_t ll = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &ll)) {
        ALOGE("getsockname: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* Subscribe to LOC server announcements: existing ones now, later ones on modem (re)start. */
    struct qrtr_ctrl_pkt lk;
    memset(&lk, 0, sizeof(lk));
    lk.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
    lk.server.service = htole32(LOC_SERVICE);
    struct sockaddr_qrtr ns = {.sq_family = AF_QIPCRTR, .sq_node = local.sq_node, .sq_port = QRTR_PORT_CTRL};
    if (sendto(fd, &lk, sizeof(lk), 0, (struct sockaddr *)&ns, sizeof(ns)) != sizeof(lk)) {
        ALOGE("qrtr lookup: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (pipe2(g.wake, O_CLOEXEC)) {
        close(fd);
        return -1;
    }
    g.fd = fd;
    g.have_peer = 0;
    if (g.cb.create_thread_cb)
        g.reader = g.cb.create_thread_cb("superman-gnss", reader_main, NULL);
    else {
        /* gps.h requires create_thread_cb; fall back for the test tool. */
        if (pthread_create(&g.reader, NULL, (void *(*)(void *))reader_main, NULL)) g.reader = 0;
    }
    if (!g.reader) {
        ALOGE("reader thread failed");
        close(fd);
        close(g.wake[0]);
        close(g.wake[1]);
        g.fd = g.wake[0] = g.wake[1] = -1;
        return -1;
    }
    g.reader_running = 1;
    if (g.cb.set_capabilities_cb) g.cb.set_capabilities_cb(GPS_CAPABILITY_SCHEDULING | GPS_CAPABILITY_SINGLE_SHOT);
    if (g.cb.set_system_info_cb) {
        GnssSystemInfo info = {.size = sizeof(info), .year_of_hw = 2014};
        g.cb.set_system_info_cb(&info);
    }
    ALOGI("initialized");
    return 0;
}

static int gps_start(void) {
    pthread_mutex_lock(&g.lock);
    g.session_wanted = 1;
    send_start_locked(); /* if the service is not up yet, NEW_SERVER sends it */
    pthread_mutex_unlock(&g.lock);
    report_status(GPS_STATUS_SESSION_BEGIN);
    ALOGI("start interval=%ums%s", g.interval_ms, g.single_shot ? " single" : "");
    return 0;
}

static int gps_stop(void) {
    struct tlvbuf b = {0};
    tlv_u8(&b, 0x01, LOC_SESSION_ID);
    pthread_mutex_lock(&g.lock);
    g.session_wanted = 0;
    send_locked(LOC_STOP, &b);
    pthread_mutex_unlock(&g.lock);
    report_status(GPS_STATUS_SESSION_END);
    ALOGI("stop");
    return 0;
}

static void gps_cleanup(void) {
    if (!g.reader_running) return;
    gps_stop();
    (void)!write(g.wake[1], "x", 1);
    pthread_join(g.reader, NULL);
    pthread_mutex_lock(&g.lock);
    close(g.fd);
    close(g.wake[0]);
    close(g.wake[1]);
    g.fd = g.wake[0] = g.wake[1] = -1;
    g.have_peer = 0;
    g.reader_running = 0;
    pthread_mutex_unlock(&g.lock);
}

static int gps_inject_time(GpsUtcTime time, int64_t ref, int uncertainty) {
    /* ref is elapsedRealtime (boot clock) at which `time` was valid. */
    int64_t now = boot_ms();
    pthread_mutex_lock(&g.lock);
    g.time_utc = time;
    g.time_boot_ms = ref > 0 && ref <= now ? ref : now;
    g.time_unc = uncertainty > 0 ? (uint32_t)uncertainty : 1000;
    int r = g.have_peer ? send_time_locked() : 0; /* else sent when LOC appears */
    pthread_mutex_unlock(&g.lock);
    ALOGI("inject time unc=%dms%s", uncertainty, r ? " (send failed)" : "");
    return r ? -1 : 0;
}

static int gps_inject_location(double lat, double lon, float accuracy) {
    struct tlvbuf b = {0};
    tlv_f64(&b, 0x10, lat);
    tlv_f64(&b, 0x11, lon);
    tlv_f32(&b, 0x12, accuracy > 0 ? accuracy : 1000.0f);
    tlv_u8(&b, 0x13, 68);
    tlv_u64(&b, 0x1B, (uint64_t)now_utc_ms());
    ALOGI("inject location acc=%.0fm", accuracy);
    return send_req(LOC_INJECT_POSITION, &b) ? -1 : 0;
}

static void gps_delete_aiding_data(GpsAidingData flags) {
    (void)flags;
    struct tlvbuf b = {0};
    tlv_u8(&b, 0x01, 1); /* delete all */
    ALOGI("delete aiding data");
    send_req(LOC_DELETE_ASSIST, &b);
}

static int gps_set_position_mode(GpsPositionMode mode, GpsPositionRecurrence recurrence,
                                 uint32_t min_interval, uint32_t pref_acc, uint32_t pref_time) {
    (void)mode; /* MS-based/assisted need SUPL, which is not configured: always standalone */
    (void)pref_acc;
    (void)pref_time;
    pthread_mutex_lock(&g.lock);
    g.interval_ms = min_interval >= 100 ? min_interval : 1000;
    g.single_shot = recurrence == GPS_POSITION_RECURRENCE_SINGLE;
    int restart = g.session_wanted;
    if (restart) send_start_locked(); /* LOC Start on a running session updates it */
    pthread_mutex_unlock(&g.lock);
    return 0;
}

static const void *gps_get_extension(const char *name) {
    (void)name;
    return NULL;
}

static const GpsInterface gps_iface = {
    .size = sizeof(GpsInterface),
    .init = gps_init,
    .start = gps_start,
    .stop = gps_stop,
    .cleanup = gps_cleanup,
    .inject_time = gps_inject_time,
    .inject_location = gps_inject_location,
    .delete_aiding_data = gps_delete_aiding_data,
    .set_position_mode = gps_set_position_mode,
    .get_extension = gps_get_extension,
};

static const GpsInterface *get_iface(struct gps_device_t *dev) {
    (void)dev;
    return &gps_iface;
}

static int dev_close(struct hw_device_t *dev) {
    free(dev);
    return 0;
}

static int open_gps(const struct hw_module_t *module, const char *id, struct hw_device_t **device) {
    (void)id;
    struct gps_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return -ENOMEM;
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = dev_close;
    dev->get_gps_interface = get_iface;
    *device = &dev->common;
    return 0;
}

static struct hw_module_methods_t gps_module_methods = {.open = open_gps};

__attribute__((visibility("default"))) struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = GPS_HARDWARE_MODULE_ID,
    .name = "Lumia 730 QMI LOC GPS",
    .author = "superman port",
    .methods = &gps_module_methods,
};

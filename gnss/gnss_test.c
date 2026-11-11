// SPDX-License-Identifier: Apache-2.0
/* superman-gnss-test [seconds] [--nmea] [--time] [--cold]
 * Drives the GPS HAL directly (no framework) and prints fixes and SV counts.
 * Do not run while vendor.gnss_service has an active session. */
#include <hardware/gps.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern struct hw_module_t HAL_MODULE_INFO_SYM;
static int show_nmea;
static time_t t0;

static void loc_cb(GpsLocation *l) {
    printf("[%3lds] FIX lat=%.6f lon=%.6f alt=%.1f acc=%.1f spd=%.1f flags=0x%x utc=%lld\n",
           (long)(time(NULL) - t0), l->latitude, l->longitude, l->altitude, l->accuracy, l->speed,
           l->flags, (long long)l->timestamp);
    fflush(stdout);
}
static void status_cb(GpsStatus *s) { printf("[%3lds] status %u\n", (long)(time(NULL) - t0), s->status); }
static void sv_cb(GnssSvStatus *s) {
    int used = 0, sig = 0;
    float best = 0;
    for (int i = 0; i < s->num_svs; i++) {
        if (s->gnss_sv_list[i].flags & GNSS_SV_FLAGS_USED_IN_FIX) used++;
        if (s->gnss_sv_list[i].c_n0_dbhz > 0) sig++;
        if (s->gnss_sv_list[i].c_n0_dbhz > best) best = s->gnss_sv_list[i].c_n0_dbhz;
    }
    printf("[%3lds] SV visible=%d with_signal=%d used=%d best_cn0=%.1f\n", (long)(time(NULL) - t0),
           s->num_svs, sig, used, best);
    for (int i = 0; i < s->num_svs; i++) {
        GnssSvInfo *v = &s->gnss_sv_list[i];
        if (v->c_n0_dbhz > 0)
            printf("        c%d sv%-3d cn0=%4.1f el=%4.1f az=%5.1f flags=%x\n", v->constellation, v->svid,
                   v->c_n0_dbhz, v->elevation, v->azimuth, v->flags);
    }
    fflush(stdout);
}
static void nmea_cb(GpsUtcTime t, const char *s, int n) {
    (void)t;
    if (show_nmea) printf("NMEA %.*s\n", n, s);
}
static void caps_cb(uint32_t c) { printf("capabilities 0x%x\n", c); }
static void nop(void) {}
static pthread_t thr_cb(const char *name, void (*start)(void *), void *arg) {
    (void)name;
    pthread_t t;
    return pthread_create(&t, NULL, (void *(*)(void *))start, arg) ? 0 : t;
}

int main(int argc, char **argv) {
    int secs = 60, inject_time = 0, cold = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nmea")) show_nmea = 1;
        else if (!strcmp(argv[i], "--time")) inject_time = 1;
        else if (!strcmp(argv[i], "--cold")) cold = 1;
        else secs = atoi(argv[i]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct hw_device_t *dev;
    if (HAL_MODULE_INFO_SYM.methods->open(&HAL_MODULE_INFO_SYM, GPS_HARDWARE_MODULE_ID, &dev)) return 1;
    const GpsInterface *gi = ((struct gps_device_t *)dev)->get_gps_interface((struct gps_device_t *)dev);
    GpsCallbacks cb = {
        .size = sizeof(cb),
        .location_cb = loc_cb,
        .status_cb = status_cb,
        .nmea_cb = nmea_cb,
        .set_capabilities_cb = caps_cb,
        .acquire_wakelock_cb = nop,
        .release_wakelock_cb = nop,
        .create_thread_cb = thr_cb,
        .gnss_sv_status_cb = sv_cb,
    };
    t0 = time(NULL);
    if (gi->init(&cb)) {
        puts("init failed");
        return 1;
    }
    sleep(1); /* let the QRTR lookup find the service */
    if (cold) gi->delete_aiding_data(GPS_DELETE_ALL);
    if (inject_time) {
        struct timespec rt, bt;
        clock_gettime(CLOCK_REALTIME, &rt);
        clock_gettime(CLOCK_BOOTTIME, &bt);
        gi->inject_time((int64_t)rt.tv_sec * 1000 + rt.tv_nsec / 1000000,
                        (int64_t)bt.tv_sec * 1000 + bt.tv_nsec / 1000000, 2000);
    }
    gi->set_position_mode(GPS_POSITION_MODE_STANDALONE, GPS_POSITION_RECURRENCE_PERIODIC, 1000, 0, 0);
    gi->start();
    sleep(secs);
    gi->stop();
    sleep(1);
    gi->cleanup();
    dev->close(dev);
    return 0;
}

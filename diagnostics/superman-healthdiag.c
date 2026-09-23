/* Independent, bounded late-boot observations for Lumia 730.
 * 61 samples, 30 seconds apart (30 minutes minimum absent clock failures).
 * No shell, Binder, logd, framebuffer access, or large memory tests.
 * Proc reads and filesystem writes may still stall: missing data is not PASS.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

static int output = -1, kernel = -1;
static size_t output_bytes;
static unsigned output_errors;
static size_t snapshot_bytes;
static unsigned snapshot_limited;

static long long uptime(void) {
    struct timespec now;
    return clock_gettime(CLOCK_BOOTTIME, &now) ? -1 : (long long)now.tv_sec;
}

static void record(int to_kernel, const char *format, ...) {
    char text[2048];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(text, sizeof(text) - 1, format, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(text) - 1) n = sizeof(text) - 2;
    text[n++] = '\n';
    /* Reserve room for all 61 heartbeats even with many matching processes. */
    if (!to_kernel) {
        if (snapshot_bytes + (size_t)n > 12 * 1024) { ++snapshot_limited; return; }
        snapshot_bytes += (size_t)n;
    }
    if (to_kernel && kernel >= 0) (void)write(kernel, text, (size_t)n);
    /* Cap attempted output as well as successful output after short writes. */
    if (output >= 0 && output_bytes + (size_t)n <= 1024 * 1024) {
        output_bytes += (size_t)n;
        ssize_t result;
        do { result = write(output, text, (size_t)n); } while (result < 0 && errno == EINTR);
        if (result != n) ++output_errors;
    }
}

/* Return bytes read or a negative errno; a full buffer may be truncated. */
static ssize_t read_text(const char *path, char *buffer, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { buffer[0] = 0; return -errno; }
    ssize_t n;
    do { n = read(fd, buffer, size - 1); } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n < 0) { buffer[0] = 0; return -saved; }
    buffer[n] = 0;
    for (ssize_t i = 0; i < n; ++i)
        if ((unsigned char)buffer[i] < 32) buffer[i] = ' ';
    return n;
}

static void sample_file(const char *path) {
    char value[1024];
    ssize_t n = read_text(path, value, sizeof(value));
    record(0, "FILE path=%s bytes=%ld limit_reached=%d value=%s", path,
           (long)n, n == (ssize_t)sizeof(value) - 1, value);
}

static int selected(const char *name) {
    static const char *names[] = {"zygote", "app_process32", "system_server",
        "bootanimation", "surfaceflinger", "composer@2.1-se", "allocator@2.0-s", "logd"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(name, names[i])) return 1;
    if (!strcmp(name, "superman-memdia") || !strcmp(name, "superman-diag-c") ||
        !strcmp(name, "superman-health")) return 2;
    return 0;
}

static void sample_processes(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) { record(0, "PROC_ERROR errno=%d", errno); return; }
    struct dirent *entry;
    unsigned scanned = 0, matches = 0;
    /* Core services first: numerous shell collector children must not crowd
     * out Zygote after it restarts with a higher PID. */
    for (int priority = 1; priority <= 2; ++priority) {
      rewinddir(dir);
      while (scanned < 4096 && matches < 16 && (entry = readdir(dir))) {
        ++scanned;
        if (!entry->d_name[0] || strspn(entry->d_name, "0123456789") != strlen(entry->d_name)) continue;
        char path[512], name[64];
        int length = snprintf(path, sizeof(path), "%s/%s/comm", root, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        if (read_text(path, name, sizeof(name)) < 0) continue;
        name[strcspn(name, " ")] = 0;
        int category = selected(name);
        if (!category && !strcmp(name, "main")) {
            char command[256];
            snprintf(path, sizeof(path), "%s/%s/cmdline", root, entry->d_name);
            if (read_text(path, command, sizeof(command)) > 0 &&
                (strstr(command, "--zygote") || !strncmp(command, "zygote", 6))) category = 1;
        }
        if (category != priority) continue;
        ++matches;
        record(0, "PROC pid=%s name=%s", entry->d_name, name);
        snprintf(path, sizeof(path), "%s/%s/stat", root, entry->d_name);
        sample_file(path);  /* includes state, parent, CPU time, starttime and RSS */
        snprintf(path, sizeof(path), "%s/%s/wchan", root, entry->d_name);
        sample_file(path);
      }
    }
    closedir(dir);
    record(0, "PROC_END entries=%u matched=%u limit_reached=%d", scanned, matches,
           scanned == 4096 || matches == 16);
}

static void sample_properties(void) {
    static const char *keys[] = {"ro.build.version.incremental", "sys.boot_completed",
        "service.bootanim.exit", "init.svc.zygote", "init.svc.bootanim",
        "init.svc.surfaceflinger", "init.svc.logd", "init.svc.superman-diag-capture",
        "init.svc.superman-memdiag", "init.svc.superman-fbdiag", "init.svc.superman-tombdiag"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX] = {0};
        int n = __system_property_get(keys[i], value);
        record(0, "PROP key=%s present=%d value=%s", keys[i], n > 0, value);
#else
        record(0, "PROP key=%s unavailable=host", keys[i]);
#endif
    }
}

static void open_output(const char *boot) {
    struct stat mount, root;
    /* Do not populate the unmounted mount-point on a failed storage boot. */
    if (stat("/lumia_sd", &mount) || stat("/", &root) || mount.st_dev == root.st_dev) return;
    if (mkdir("/lumia_sd/superman-v70", 0700) && errno != EEXIST) return;
    char path[192];
    snprintf(path, sizeof(path), "/lumia_sd/superman-v70/health-%s-%ld.txt", boot, (long)getpid());
    output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) { record(1, "HEALTH_OPEN_ERROR errno=%d", errno); return; }
    record(1, "HEALTH_OPEN path=%s uptime=%lld tick_seconds=30 samples=61 byte_limit=1048576",
           path, uptime());
    sample_file("/system/etc/superman-v70-build-manifest.txt");
}

int main(void) {
    char boot[64];
    if (read_text("/proc/sys/kernel/random/boot_id", boot, sizeof(boot)) <= 0) return 1;
    boot[strcspn(boot, " ")] = 0;
    if (strlen(boot) != 36 || strspn(boot, "0123456789abcdef-") != 36) return 1;
    kernel = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    record(1, "HEALTH_START boot=%s pid=%ld uptime=%lld hz=%ld page_size=%ld",
           boot, (long)getpid(), uptime(), sysconf(_SC_CLK_TCK), sysconf(_SC_PAGESIZE));
    for (unsigned tick = 0; tick <= 60; ++tick) {
        snapshot_bytes = 0;
        snapshot_limited = 0;
        if (output < 0) open_output(boot);
        record(1, "HEALTH_BEGIN tick=%u uptime=%lld output_errors=%u", tick, uptime(), output_errors);
        sample_properties();
        sample_file("/proc/meminfo");
        sample_file("/proc/loadavg");
        sample_file("/proc/pressure/cpu");
        sample_file("/proc/pressure/io");
        sample_file("/proc/pressure/memory");
        /* Fixed, read-only paths: deltas expose SD throughput, request wait
         * time and queue pressure without issuing any probe IO. */
        sample_file("/sys/class/block/mmcblk1/stat");
        sample_file("/sys/class/block/mmcblk1/inflight");
        sample_file("/sys/class/block/mmcblk1/mmcblk1p1/stat");
        sample_file("/sys/class/block/mmcblk1/mmcblk1p3/stat");
        sample_file("/sys/devices/system/cpu/online");
        sample_processes("/proc");
        record(1, "HEALTH_END tick=%u uptime=%lld bytes=%zu output_errors=%u omitted_records=%u",
               tick, uptime(), output_bytes, output_errors, snapshot_limited);
        if (output >= 0) {
            long long started = uptime();
            record(1, "HEALTH_SYNC_BEGIN tick=%u uptime=%lld", tick, started);
            int result = fsync(output);
            int saved = result ? errno : 0;
            if (result) ++output_errors;
            record(1, "HEALTH_SYNC_END tick=%u uptime=%lld elapsed_seconds=%lld rc=%d errno=%d",
                   tick, uptime(), uptime() - started, result, saved);
        }
        if (tick < 60) {
            struct timespec delay = {30, 0};
            while (nanosleep(&delay, &delay) && errno == EINTR) {}
        }
    }
    record(1, "HEALTH_FINISHED uptime=%lld", uptime());
    if (output >= 0) { (void)fsync(output); close(output); }
    if (kernel >= 0) close(kernel);
    return 0;
}

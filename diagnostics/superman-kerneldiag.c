/* Independent raw /dev/kmsg capture. Preserves sequence/timestamp headers,
 * including ART and init crash messages. Does not clear the kernel ring.
 * Bounded to 30 minutes / 16 MiB. SD writes can still block or be lost on reset.
 */
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/syscall.h>
#include "superman-sha256.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static long seconds(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : t.tv_sec;
}
static int put(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
/* Only exceptional records bypass batching; normal capture retains its cadence.
 * Sync may still stall on faulty media and cannot guarantee reset survival. */
static int urgent_record(const char *record) {
    return strstr(record, "result=FAIL") || strstr(record, "ANON_SNAPSHOT") ||
           strstr(record, "Fatal signal") || strstr(record, "Kernel panic") ||
           strstr(record, "InitFatalReboot") || strstr(record, "Restarting system") ||
           strstr(record, "watchdog bite");
}

/* Match the exact preserved r37 ramdisk wrapper, never Android init. A pidfd
 * prevents signalling a recycled PID. Missing pidfd support leaves it running.
 * Pause only after our first successful data flush; resume on normal/error exit.
 */
static int wrapper_matches(int proc) {
    char comm[32] = {0}, data[4096], hex[65];
    int fd = openat(proc, "comm", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, comm, sizeof(comm)-1);
    if (fd >= 0) close(fd);
    if (n <= 0 || strcmp(comm, "lumia-a11-log\n")) return 0;
    fd = openat(proc, "exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != 463984) {
        close(fd); return 0;
    }
    struct sha256_ctx ctx;
    unsigned char digest[32];
    sha256_init(&ctx);
    size_t total = 0;
    while (total < 463984) {
        n = read(fd, data, sizeof(data));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sha256_update(&ctx, data, (size_t)n); total += (size_t)n;
    }
    close(fd);
    if (total != 463984) return 0;
    sha256_final(&ctx, digest);
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(hex + 2*i, 3, "%02x", digest[i]);
    return !strcmp(hex, "15d05d1072d3f0e133be4214aaf77ddf0b10b42006a447e06b60d63b7d737bc7");
}

static int pause_legacy_logger(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return -1;
    struct dirent *entry;
    unsigned scanned = 0;
    int paused = -1;
    while (scanned++ < 4096 && (entry = readdir(dir))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 1 || pid > 4194304 || pid == getpid()) continue;
        int target = (int)syscall(__NR_pidfd_open, pid, 0);
        if (target < 0) continue;
        int proc = openat(dirfd(dir), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (proc < 0) { close(target); continue; }
        if (target >= 0) {
            struct pollfd alive = {target, POLLIN, 0};
            if (wrapper_matches(proc) && poll(&alive, 1, 0) == 0 &&
                syscall(__NR_pidfd_send_signal, target, SIGSTOP, NULL, 0) == 0) {
                paused = target;
                close(proc);
                break;
            }
            close(target);
        }
        close(proc);
    }
    closedir(dir);
    return paused;
}

static void handoff_note(int kernel, const char *state) {
    char line[160];
    int n = snprintf(line, sizeof(line), "<6>SUPERMAN_LOG_HANDOFF state=%s uptime=%ld\n", state, seconds());
    if (kernel >= 0 && n > 0 && (size_t)n < sizeof(line)) (void)write(kernel, line, (size_t)n);
}
static volatile sig_atomic_t stopping;
static void stop_capture(int signal_number) {
    (void)signal_number;
    stopping = 1;
}
int main(void) {
    char boot[80], path[256], buffer[8192];
    /* Batch burst records before SD IO. Flush after at most one second idle
     * or at 64 KiB; sync every five seconds. Storage stalls remain possible. */
    static char pending[65536];
    size_t pending_size = 0;
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, boot, sizeof(boot)-1);
    if (fd >= 0) close(fd);
    if (n <= 0) return 1;
    boot[n] = 0; boot[strcspn(boot, "\r\n")] = 0;
    (void)mkdir("/lumia_sd/superman-v70", 0700);
    struct statvfs space;
    if (statvfs("/lumia_sd/superman-v70", &space) ||
        (unsigned long long)space.f_bavail * space.f_frsize < 32ULL*1024*1024) return 2;
    snprintf(path, sizeof(path), "/lumia_sd/superman-v70/kernel-live-%s-%ld.txt",
             boot, (long)getpid());
    int input = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) return 3;
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) { close(input); return 4; }
    int kernel = open("/dev/kmsg", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    int legacy = -1, handoff_attempted = 0;
    struct sigaction action = {0};
    action.sa_handler = stop_capture;
    sigemptyset(&action.sa_mask);
    int can_handoff = sigaction(SIGTERM, &action, NULL) == 0 &&
                      sigaction(SIGINT, &action, NULL) == 0;
    int directory = open("/lumia_sd/superman-v70", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int directory_ready = directory >= 0 && fsync(directory) == 0;
    if (directory >= 0) close(directory);
    can_handoff = can_handoff && directory_ready && kernel >= 0;
    long start = seconds(), synced = start, flushed = start;
    size_t bytes = 0;
    int status = 0;
    unsigned urgent_syncs = 0;
    while (!stopping && start >= 0 && seconds() >= start && seconds()-start < 1800 && bytes < 16*1024*1024) {
        struct pollfd p = {input, POLLIN, 0};
        int ready = poll(&p, 1, 250);
        if (ready < 0 && errno != EINTR) { status = 5; break; }
        if (ready > 0) {
            n = read(input, buffer, sizeof(buffer)-1);
            if (n < 0 && errno == EPIPE) {
                const char gap[] = "CAPTURE_GAP kernel ring overrun\n";
                memcpy(buffer, gap, sizeof(gap)-1);
                n = sizeof(gap)-1;
            } else if (n > 0) {
                if (bytes + (size_t)n > 16*1024*1024) break;
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                status = 7; break;
            }
            if (n > 0) {
                if (bytes + pending_size + (size_t)n > 16*1024*1024) break;
                if (pending_size + (size_t)n > sizeof(pending)) {
                    if (put(output, pending, pending_size)) { status = 6; break; }
                    bytes += pending_size; pending_size = 0; flushed = seconds();
                }
                memcpy(pending + pending_size, buffer, (size_t)n);
                pending_size += (size_t)n;
                buffer[n] = 0;
                if (urgent_record(buffer) && urgent_syncs < 8) {
                    if (put(output, pending, pending_size)) { status = 6; break; }
                    bytes += pending_size; pending_size = 0; flushed = seconds();
                    if (fdatasync(output)) { status = 8; break; }
                    synced = seconds();
                    ++urgent_syncs;
                }
            }
        }
        long now = seconds();
        if (pending_size && now - flushed >= 1) {
            if (put(output, pending, pending_size)) { status = 6; break; }
            bytes += pending_size; pending_size = 0; flushed = now;
        }
        if (now - synced >= 5) {
            if (fdatasync(output)) { status = 8; break; }
            synced = now;
            if (!handoff_attempted && bytes > 0) {
                handoff_attempted = 1;
                handoff_note(kernel, "replacement-flushed");
                if (can_handoff) legacy = pause_legacy_logger("/proc");
                handoff_note(kernel, legacy >= 0 ? "legacy-stop-requested" : "legacy-retained");
            }
        }
    }
    /* Restore early capture before final IO, which can itself block. */
    if (legacy >= 0) {
        int rc = (int)syscall(__NR_pidfd_send_signal, legacy, SIGCONT, NULL, 0);
        handoff_note(kernel, rc == 0 ? "legacy-resume-requested" : "legacy-resume-failed");
        close(legacy);
    }
    if (kernel >= 0) close(kernel);
    if (status == 0 && pending_size && put(output, pending, pending_size)) status = 6;
    if (fsync(output)) status = 8;
    close(output); close(input);
    return status;
}

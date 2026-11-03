/*
 * Early, dependency-free ARM32 runtime-integrity diagnostic.
 *
 * This deliberately uses a static executable and writes independently to
 * /dev/kmsg and the per-boot capture directory.  It does not start, stop, or
 * signal any Android service.
 */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "superman-sha256.h"

static int out_fd = -1;
static int kmsg_fd = -1;
static time_t last_log_sync;

/* Event-driven batching: first record after 30 seconds, plus failures/end.
 * Explicit fault, extraction and final flushes retain their own semantics. */
static void flush_log(void) {
    if (out_fd >= 0) (void)fsync(out_fd);
    struct timespec now;
    if (!clock_gettime(CLOCK_BOOTTIME, &now)) last_log_sync = now.tv_sec;
}

/* Inherited across fork, including faults before the child branch executes.
 * Stages: 1=fork, 2=child entry, 3=byte samples, 4=read/hash,
 * 5=mmap/hash, 6=anonymous test, 7=COW test, 8=worker completion.
 * Worker: 0=initial parent, 1=file, 2=repeated anonymous; target is file index.
 */
static volatile sig_atomic_t fault_stage, fault_worker, fault_target, fault_run;
static unsigned char fault_stack[64 * 1024] __attribute__((aligned(16)));

static void fault_text(char *buf, size_t *n, const char *s) {
    while (*s && *n < 2046) buf[(*n)++] = *s++;
}

static void fault_value(char *buf, size_t *n, const char *name, uintptr_t value) {
    static const char digits[] = "0123456789abcdef";
    fault_text(buf, n, name);
    fault_text(buf, n, "=0x");
    for (unsigned i = sizeof(value) * 2; i > 0 && *n < 2046; --i)
        buf[(*n)++] = digits[(value >> ((i - 1) * 4)) & 15];
    fault_text(buf, n, " ");
}

static void capture_fault(int sig, siginfo_t *info, void *context) {
    /* No printf, allocation, unwinding, or reads through faulting pointers.
     * Only bounded formatting and async-signal-safe I/O on existing fds.
     */
    char record[2048];
    size_t n = 0;
    fault_text(record, &n, "<3>SUPERMAN_MEMDIAG FAULT ");
    fault_value(record, &n, "pid", (uintptr_t)getpid());
    fault_value(record, &n, "signal", (uintptr_t)sig);
    fault_value(record, &n, "code", (uintptr_t)info->si_code);
    fault_value(record, &n, "address", (uintptr_t)info->si_addr);
    fault_value(record, &n, "worker", (uintptr_t)fault_worker);
    fault_value(record, &n, "target", (uintptr_t)fault_target);
    fault_value(record, &n, "run", (uintptr_t)fault_run);
    fault_value(record, &n, "stage", (uintptr_t)fault_stage);
#if defined(__arm__)
    const ucontext_t *uc = context;
#define FAULT_REG(field) fault_value(record, &n, #field, uc->uc_mcontext.field)
    FAULT_REG(arm_r0); FAULT_REG(arm_r1); FAULT_REG(arm_r2); FAULT_REG(arm_r3);
    FAULT_REG(arm_r4); FAULT_REG(arm_r5); FAULT_REG(arm_r6); FAULT_REG(arm_r7);
    FAULT_REG(arm_r8); FAULT_REG(arm_r9); FAULT_REG(arm_r10); FAULT_REG(arm_fp);
    FAULT_REG(arm_ip); FAULT_REG(arm_sp); FAULT_REG(arm_lr); FAULT_REG(arm_pc);
    FAULT_REG(arm_cpsr); FAULT_REG(fault_address);
#undef FAULT_REG
#else
    (void)context;
    fault_text(record, &n, "registers=unsupported-host ");
#endif
    record[n++] = '\n';
    if (kmsg_fd >= 0) (void)write(kmsg_fd, record, n);
    if (out_fd >= 0) {
        (void)write(out_fd, record, n);
        (void)fsync(out_fd);
    }
    /* SA_RESETHAND restored the default action. Deliver after handler return
     * so waitpid still reports the original signal, not a synthetic exit code.
     */
    (void)kill(getpid(), sig);
}

static int install_fault_capture(void) {
    stack_t stack = { .ss_sp = fault_stack, .ss_size = sizeof(fault_stack) };
    struct sigaction action = {0};
    const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    if (sigaltstack(&stack, NULL) != 0) return -1;
    action.sa_sigaction = capture_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        if (sigaction(signals[i], &action, NULL) != 0) return -1;
    return 0;
}

static void emit(const char *fmt, ...) {
    char line[768];
    static const char prefix[] = "<6>SUPERMAN_MEMDIAG ";
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    if (kmsg_fd >= 0) {
        char record[sizeof(line) + sizeof(prefix) + 1];
        size_t len = sizeof(prefix) - 1;
        memcpy(record, prefix, len);
        memcpy(record + len, line, (size_t)n);
        len += (size_t)n;
        record[len++] = '\n';
        /* Each write to /dev/kmsg is a separate kernel log record. */
        (void)write(kmsg_fd, record, len);
    }
    if (out_fd >= 0) {
        /* One append write prevents parent/worker records interleaving. */
        struct timespec now;
        int timed = clock_gettime(CLOCK_BOOTTIME, &now) == 0;
        int urgent = strstr(line, "FAIL") || strstr(line, "ERROR") ||
                     strstr(line, "ANON_SNAPSHOT") || strstr(line, "rc=-") ||
                     !strncmp(line, "END ", 4) || !strncmp(line, "TS_END ", 7) ||
                     !strcmp(line, "FB_END");
        line[n++] = '\n';
        (void)write(out_fd, line, (size_t)n);
        if (urgent || !timed || now.tv_sec - last_log_sync >= 30) flush_log();
    }
}

static void hex32(const uint8_t *in, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned i;
    for (i = 0; i < 32; ++i) { out[i * 2] = hex[in[i] >> 4]; out[i * 2 + 1] = hex[in[i] & 15]; }
    out[64] = 0;
}

/* Bounded independent capture; hex preserves bytes and record boundaries.
 * The same process and output media can still fail: absence is not a PASS.
 */
static void capture_memory_map(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { emit("MAP_ERROR path=%s errno=%d", path, errno); return; }
    size_t total = 0;
    uint8_t bytes[64];
    emit("MAP_BEGIN path=%s limit=16384 encoding=hex", path);
    while (total < 16384) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            emit("MAP_END path=%s bytes=%zu eof=%d errno=%d", path, total,
                 n == 0, n < 0 ? errno : 0);
            close(fd);
            return;
        }
        char hex[129];
        static const char digits[] = "0123456789abcdef";
        for (ssize_t j = 0; j < n; ++j) {
            hex[2*j] = digits[bytes[j] >> 4];
            hex[2*j+1] = digits[bytes[j] & 15];
        }
        hex[2*n] = 0;
        emit("MAP_DATA path=%s offset=%zu hex=%s", path, total, hex);
        total += (size_t)n;
    }
    emit("MAP_END path=%s bytes=%zu limit_reached=1 eof=unknown", path, total);
    close(fd);
}

static int hash_read(const char *path, off_t *size, char digest[65]) {
    fault_stage = 4;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    uint8_t buf[32768], sum[32];
    ssize_t n;
    struct sha256_ctx c;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return -errno; }
    sha256_init(&c);
    off_t total = 0, next_progress = 4 * 1024 * 1024;
    if (st.st_size >= next_progress)
        emit("READ_SIZE path=%s bytes=%lld", path, (long long)st.st_size);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sha256_update(&c, buf, (size_t)n);
        total += n;
        if (total >= next_progress) {
            struct timespec now = {0};
            (void)clock_gettime(CLOCK_BOOTTIME, &now);
            emit("READ_PROGRESS path=%s bytes=%lld uptime=%lld", path,
                 (long long)total, (long long)now.tv_sec);
            next_progress = total + 4 * 1024 * 1024;
        }
    }
    if (n < 0) { int e = errno; close(fd); return -e; }
    sha256_final(&c, sum); hex32(sum, digest); *size = st.st_size; close(fd); return 0;
}

static int hash_mmap(const char *path, off_t *size, char digest[65]) {
    fault_stage = 5;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    uint8_t sum[32];
    struct sha256_ctx c;
    size_t i, page;
    volatile const uint8_t *p;
    if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return -errno; }
    *size = st.st_size;
    if (st.st_size == 0) { close(fd); sha256_init(&c); sha256_final(&c, sum); hex32(sum, digest); return 0; }
    p = (volatile const uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { int e = errno; close(fd); return -e; }
    sha256_init(&c);
    page = (size_t)sysconf(_SC_PAGESIZE); if (page == 0) page = 4096;
    for (i = 0; i < (size_t)st.st_size; i += page) {
        uint8_t v = p[i];
        sha256_update(&c, &v, 0);
    }
    /* Hash the complete mapping after the page-by-page fault walk. */
    sha256_init(&c);
    sha256_update(&c, (const void *)p, (size_t)st.st_size);
    sha256_final(&c, sum); hex32(sum, digest);
    munmap((void *)p, (size_t)st.st_size); close(fd); return 0;
}

struct target { const char *path; const char *expected; };
static const struct target targets[] = {
    { "/system/bin/crash_dump32", "eb279056605d49a048421cec1aaae62867ae9bc9353d5e998fa06c8641c897e9" },
    { "/system/bin/app_process32", "6b0d00a980b0d33b6d49a873ab8e8a534c4baf01c9452ea48f439df960dad200" },
    { "/system/apex/com.android.runtime/bin/linker", "c295522e7fea92a20b88fe824ce805ea15daca4ef370f333628b8e45cd8bf0e2" },
    { "/apex/com.android.runtime/bin/linker", "c295522e7fea92a20b88fe824ce805ea15daca4ef370f333628b8e45cd8bf0e2" },
    { "/system/apex/com.android.runtime/lib/bionic/libc.so", "09dd4a16ef44e342459fa06eb80a69567c0f84111a9b53be7d499d2ed01c4548" },
    { "/apex/com.android.runtime/lib/bionic/libc.so", "09dd4a16ef44e342459fa06eb80a69567c0f84111a9b53be7d499d2ed01c4548" },
    { "/system/lib/libc++.so", "f9ed4067baf0baede686252c6df1fd84354eda47fd0a6923f253d0c1134b595c" },
    { "/system/apex/com.android.art.debug/lib/libart.so", "6f304ea97c071a41ff157c032ecbe8355235648f90a822c30e8a5cfdfadf1085" },
    { "/apex/com.android.art/lib/libart.so", "6f304ea97c071a41ff157c032ecbe8355235648f90a822c30e8a5cfdfadf1085" },
};

/* Small, repeatable windows spanning the file, including header and EOF.
 * Emit actual bytes even when the access methods agree: both can be wrong.
 * These are sampled regions, not a proof of whole-file integrity. */
static void sample_file_bytes(const char *path) {
    fault_stage = 3;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) { emit("BYTE_ERROR path=%s open_errno=%d", path, errno); return; }
    if (fstat(fd, &st) || st.st_size < 64) {
        emit("BYTE_ERROR path=%s invalid_size_or_stat", path); close(fd); return;
    }
    emit("BYTE_BEGIN path=%s size=%lld windows=16 bytes_per_window=64", path, (long long)st.st_size);
    for (unsigned window = 0; window < 16; ++window) {
        off_t offset = (st.st_size - 64) / 15 * window;
        if (window == 15) offset = st.st_size - 64;
        uint8_t values[3][64];
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) { emit("BYTE_ERROR invalid_pagesize"); break; }
        off_t base = offset / page * page;
        size_t delta = (size_t)(offset - base);
        ssize_t n = pread(fd, values[0], 64, offset);
        if (n != 64) { emit("BYTE_ERROR path=%s offset=%lld read1=%lld",path,(long long)offset,(long long)n); continue; }
        volatile const uint8_t *p = mmap(NULL, delta + 64, PROT_READ, MAP_PRIVATE, fd, base);
        if (p == MAP_FAILED) { emit("BYTE_ERROR path=%s offset=%lld mmap_errno=%d",path,(long long)offset,errno); continue; }
        for (unsigned j = 0; j < 64; ++j) values[1][j] = p[delta+j];
        munmap((void *)p, delta + 64);
        n = pread(fd, values[2], 64, offset);
        if (n != 64) { emit("BYTE_ERROR path=%s offset=%lld read2=%lld",path,(long long)offset,(long long)n); continue; }
        for (unsigned method = 0; method < 3; ++method) {
            char hex[129];
            hex32(values[method], hex);
            hex32(values[method]+32, hex+64);
            emit("BYTE_SAMPLE path=%s offset=%lld method=%s hex=%s",path,(long long)offset,
                 method == 0 ? "read1" : method == 1 ? "mmap" : "read2",hex);
        }
    }
    close(fd);
    emit("BYTE_END path=%s", path);
}

static void test_file(const struct target *t) {
    off_t rs = 0, ms = 0;
    char rh[65], mh[65];
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_BOOTTIME, &now);
    emit("FILE_BEGIN path=%s phase=read uptime=%lld", t->path, (long long)now.tv_sec);
    int rr = hash_read(t->path, &rs, rh);
    emit("FILE_READ path=%s rc=%d sha256=%s", t->path, rr, rr == 0 ? rh : "unavailable");
    emit("FILE_BEGIN path=%s phase=mmap", t->path);
    int mr = hash_mmap(t->path, &ms, mh);
    if (rr != 0 || mr != 0) {
        emit("FILE path=%s read_rc=%d mmap_rc=%d read_size=%lld mmap_size=%lld", t->path,
             rr, mr, (long long)rs, (long long)ms);
        return;
    }
    emit("FILE path=%s size=%lld read_sha256=%s mmap_sha256=%s historical_reference=%s read_matches_reference=%s mmap_matches_reference=%s same=%s",
         t->path, (long long)rs, rh, mh, t->expected,
         !strcmp(t->expected, "unavailable") ? "unavailable" : strcmp(rh, t->expected) == 0 ? "yes" : "no",
         !strcmp(t->expected, "unavailable") ? "unavailable" : strcmp(mh, t->expected) == 0 ? "yes" : "no",
         strcmp(rh, mh) == 0 && rs == ms ? "yes" : "no");
}

static void log_page_metadata(const volatile uint8_t *address) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t virtual_address = (uintptr_t)address;
    uint64_t entry = 0;
    unsigned char resident = 0;
    int fd, pagemap_rc = -1, saved_errno = 0;
    if (page <= 0) page = 4096;
    fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        off_t offset = (off_t)((virtual_address / (uintptr_t)page) * sizeof(entry));
        ssize_t n = pread(fd, &entry, sizeof(entry), offset);
        pagemap_rc = n == sizeof(entry) ? 0 : -1;
        if (pagemap_rc) { saved_errno = n < 0 ? errno : EIO; entry = 0; }
        close(fd);
    } else saved_errno = errno;
    int mincore_rc = mincore((void *)(virtual_address & ~((uintptr_t)page - 1)),
                             (size_t)page, &resident);
    int mincore_errno = mincore_rc ? errno : 0;
    emit("ANON_PAGE virtual=%p page_size=%ld pagemap_rc=%d pagemap_errno=%d entry=%016llx present=%u swapped=%u pfn=%llx mincore_rc=%d mincore_errno=%d resident=%u",
         (const void *)address, page, pagemap_rc, saved_errno,
         (unsigned long long)entry, (unsigned)((entry >> 63) & 1),
         (unsigned)((entry >> 62) & 1),
         (unsigned long long)(entry & ((1ULL << 55) - 1)),
         mincore_rc, mincore_errno, (unsigned)(resident & 1));
    emit("ANON_PAGE_ATTRIBUTION virtual=%p status=%s",
         (const void *)address,
         pagemap_rc || !(entry & (1ULL << 63)) || (entry & (1ULL << 62))
             ? "unavailable"
             : !(entry & ((1ULL << 55) - 1))
                 ? "zero-PFN-unresolved-no-physical-attribution"
                 : "nonzero-PFN-reported-unverified");
}

/* Supplemental page experiment. Same allocation and pattern in both contexts;
 * first verify each page immediately, then rescan after all pages are written.
 * Keep summaries bounded: detailed metadata for at most four failing pages.
 */
static void test_page_sequence(unsigned run, const char *role) {
    const size_t len = 16 * 1024 * 1024;
    long ps = sysconf(_SC_PAGESIZE);
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_BOOTTIME, &now);
    emit("PAGE_BEGIN run=%u role=%s pid=%ld uptime=%lld page_size=%ld",
         run, role, (long)getpid(), (long long)now.tv_sec, ps);
    if (ps < 64 || len % (size_t)ps) { emit("PAGE_ERROR invalid_page_size"); return; }
    fault_stage = 9;
    volatile uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { emit("PAGE_ERROR mmap_errno=%d", errno); return; }
    unsigned failures[2] = {0, 0};
    for (unsigned phase = 0; phase < 2; ++phase) {
        fault_stage = phase ? 11 : 10;
        for (size_t base = 0; base < len; base += (size_t)ps) {
            if ((base / (size_t)ps) % 256 == 0) {
                (void)clock_gettime(CLOCK_BOOTTIME, &now);
                emit("PAGE_PROGRESS run=%u role=%s pid=%ld phase=%u next_page=%zu uptime=%lld",
                     run, role, (long)getpid(), phase, base / (size_t)ps,
                     (long long)now.tv_sec);
            }
            if (!phase)
                for (size_t j = 0; j < (size_t)ps; ++j)
                    p[base + j] = (uint8_t)(1 + ((base / (size_t)ps + j) % 255));
            size_t bad = (size_t)ps, zeros = 0, mismatches = 0;
            for (size_t j = 0; j < (size_t)ps; ++j) {
                uint8_t actual = p[base + j];
                uint8_t expected = (uint8_t)(1 + ((base / (size_t)ps + j) % 255));
                zeros += actual == 0;
                if (actual != expected) { if (!mismatches) bad = j; ++mismatches; }
            }
            if (mismatches && failures[phase]++ < 4) {
                /* Snapshot before logging/metadata; a reread, not the original
                 * failing observation. Clip the 64-byte window to this page. */
                size_t start = bad > 16 ? bad - 16 : 0;
                if (start + 64 > (size_t)ps) start = (size_t)ps - 64;
                uint8_t actual_bytes[64], expected_bytes[64];
                char actual_hex[129], expected_hex[129];
                for (size_t j = 0; j < 64; ++j) {
                    actual_bytes[j] = p[base + start + j];
                    expected_bytes[j] = (uint8_t)(1 + ((base / (size_t)ps + start + j) % 255));
                }
                hex32(actual_bytes, actual_hex);
                hex32(actual_bytes + 32, actual_hex + 64);
                hex32(expected_bytes, expected_hex);
                hex32(expected_bytes + 32, expected_hex + 64);
                emit("PAGE_FAIL run=%u role=%s phase=%u base=%p offset=%zu first=%zu mismatches=%zu zeros=%zu reread=%02x",
                     run, role, phase, (void *)p, base, bad, mismatches, zeros,
                     (unsigned)p[base + bad]);
                emit("PAGE_BYTES run=%u role=%s phase=%u offset=%zu length=64 actual_reread=%s expected=%s",
                     run, role, phase, base + start, actual_hex, expected_hex);
                log_page_metadata(p + base);
                if (base) log_page_metadata(p + base - (size_t)ps);
                if (base + (size_t)ps < len) log_page_metadata(p + base + (size_t)ps);
            }
        }
        emit("PAGE_PHASE run=%u role=%s phase=%u failing_pages=%u total_pages=%zu",
             run, role, phase, failures[phase], len / (size_t)ps);
    }
    munmap((void *)p, len);
    (void)clock_gettime(CLOCK_BOOTTIME, &now);
    emit("PAGE_END run=%u role=%s uptime=%lld", run, role, (long long)now.tv_sec);
}

/* Save the failing neighborhood before slow logging can obscure an overwrite.
 * The copy is sequential, not atomic; never interpret a zero PFN as RAM zero. */
static void capture_anon_failure(const volatile uint8_t *p, size_t len,
                                 size_t offset, unsigned pass) {
    uint8_t saved[64], reread[64], via_proc[64];
    char hex[129], second_hex[129], proc_hex[129];
    size_t start = offset > 16 ? offset - 16 : 0;
    size_t count = len - start < sizeof(saved) ? len - start : sizeof(saved);
    for (size_t j = 0; j < count; ++j) saved[j] = p[start + j];
    struct timespec captured = {0};
    (void)clock_gettime(CLOCK_BOOTTIME, &captured);
    /* Both direct observations precede emit(), which may block in fsync.
     * proc mem is another access path to the same memory, not an independent
     * hardware oracle. pread64 avoids ARM32 off_t truncation above 2 GiB. */
    for (size_t j = 0; j < count; ++j) reread[j] = p[start + j];
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    ssize_t proc_count = -1;
    int proc_errno = fd < 0 ? errno : 0;
    if (fd >= 0) {
        proc_count = pread64(fd, via_proc, count, (off64_t)(uintptr_t)(p + start));
        if (proc_count < 0) proc_errno = errno;
        close(fd);
    }
    struct timespec finished = {0};
    (void)clock_gettime(CLOCK_BOOTTIME, &finished);
    for (size_t j = 0; j < count; ++j) snprintf(hex + 2*j, 3, "%02x", saved[j]);
    hex[2*count] = 0;
    emit("ANON_SNAPSHOT offset=%zu bytes=%zu pass=%u boottime=%lld.%09ld hex=%s",
         start, count, pass, (long long)captured.tv_sec, captured.tv_nsec, hex);
    unsigned changed = 0, mismatches = 0;
    for (size_t j = 0; j < count; ++j) {
        if (saved[j] != (uint8_t)(pass * 0x31 + start + j)) ++mismatches;
        if (reread[j] != saved[j]) ++changed;
        snprintf(second_hex + 2*j, 3, "%02x", reread[j]);
    }
    emit("ANON_SNAPSHOT_COMPARE saved_mismatches=%u changed_on_reread=%u bytes=%zu",
         mismatches, changed, count);
    second_hex[2*count] = 0;
    emit("ANON_REREAD timing=before-logging offset=%zu bytes=%zu hex=%s", start, count, second_hex);
    proc_hex[0] = 0;
    if (proc_count > 0)
        for (ssize_t j = 0; j < proc_count; ++j)
            snprintf(proc_hex + 2*j, 3, "%02x", via_proc[j]);
    emit("ANON_PROC_MEM offset=%zu requested=%zu read=%lld errno=%d finished=%lld.%09ld hex=%s",
         start, count, (long long)proc_count, proc_errno,
         (long long)finished.tv_sec, finished.tv_nsec, proc_hex);
}

static void test_anonymous(unsigned run) {
    fault_stage = 6;
    emit("ANON_BEGIN run=%u length=16777216", run);
    const size_t len = 16 * 1024 * 1024;
    volatile uint8_t *p = (volatile uint8_t *)mmap(NULL, len, PROT_READ | PROT_WRITE,
                                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t i;
    unsigned pass;
    if (p == MAP_FAILED) { emit("ANON mmap_rc=%d errno=%d", -errno, errno); return; }
    for (pass = 0; pass < 4; ++pass) {
        for (i = 0; i < len; ++i) p[i] = (uint8_t)(pass * 0x31 + i);
        for (i = 0; i < len; ++i) {
            uint8_t actual = p[i];
            uint8_t expected = (uint8_t)(pass * 0x31 + i);
            if (actual == expected) continue;
            capture_anon_failure(p, len, i, pass);
            emit("ANON pass=%u run=%u result=FAIL base=%p length=%zu offset=%zu address=%p expected=%02x actual=%02x",
                 pass, run, (void *)p, len, i, (void *)(p + i), expected, actual);
            log_page_metadata(p + i);
            capture_memory_map("/proc/self/status");
            size_t start = i > 16 ? i - 16 : 0;
            size_t end = i + 17 < len ? i + 17 : len;
            for (size_t j = start; j < end; ++j)
                emit("ANON_NEAR offset=%zu expected=%02x reread=%02x", j,
                     (unsigned)(uint8_t)(pass * 0x31 + j), (unsigned)p[j]);
            munmap((void *)p, len); return;
        }
    }
    emit("ANON run=%u length=%zu passes=4 result=PASS", run, len);
    munmap((void *)p, len);
}

/* A bounded private-mapping isolation check, independent of Android services.
 * Child exits without stdio/allocator cleanup and never writes capture files.
 * This checks anonymous COW only, not file relocation or general TLS health. */
static void test_cow(void) {
    fault_stage = 7;
    const size_t len = 1024 * 1024;
    volatile uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    emit("COW_BEGIN length=%zu", len);
    if (p == MAP_FAILED) { emit("COW result=ERROR errno=%d", errno); return; }
    for (size_t i = 0; i < len; ++i) p[i] = (uint8_t)(i * 17 + 31);
    pid_t child = fork();
    if (child == 0) {
        alarm(10);
        for (size_t i = 0; i < len; ++i)
            if (p[i] != (uint8_t)(i * 17 + 31)) _exit(2);
        for (size_t i = 0; i < len; ++i) p[i] = (uint8_t)(i * 29 + 7);
        for (size_t i = 0; i < len; ++i)
            if (p[i] != (uint8_t)(i * 29 + 7)) _exit(3);
        _exit(0);
    }
    if (child < 0) {
        emit("COW fork result=ERROR errno=%d", errno);
    } else {
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        size_t i;
        for (i = 0; i < len; ++i)
            if (p[i] != (uint8_t)(i * 17 + 31)) break;
        emit("COW result=%s child_status=%d parent_first_mismatch=%zu length=%zu",
             waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && i == len
                 ? "PASS" : "FAIL", status, i, len);
    }
    munmap((void *)p, len);
}

static int open_output(void) {
    char boot[80], path[256];
    struct stat st;
    unsigned wait;
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    emit("START pid=%ld uid=%ld", (long)getpid(), (long)getuid());
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, boot, sizeof(boot) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) strcpy(boot, "unknown"); else { boot[n] = 0; boot[strcspn(boot, "\r\n")] = 0; }
    (void)mkdir("/data", 0755);
    (void)mkdir("/data/superman-v70", 0755);
    snprintf(path, sizeof(path), "/data/superman-v70/boot-%s", boot);
    /* The capture script owns this directory and may choose -1, -2, ... if
     * an earlier process already created the plain boot directory.  Wait for
     * its directory to appear; never create it here. */
    for (wait = 0; wait < 30; ++wait) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) break;
        sleep(1);
    }
    if (wait == 30) snprintf(path, sizeof(path), "/data/superman-v70");
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/memdiag-v70-%ld.txt", (long)getpid());
    out_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    emit("OUTPUT boot_id=%s path=%s fd=%d", boot, path, out_fd);
    return out_fd >= 0 ? 0 : -errno;
}

static int write_all(int fd, const void *buf, size_t size) {
    const char *p = buf;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

/* Independent init service: no shell, Binder or Android dynamic libraries.
 * Preserve partial files. Record geometry so raw scanout can be decoded. */
static int capture_framebuffer(void) {
    char boot[80], dir[256], path[320], final[320];
    int id = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = id < 0 ? -1 : read(id, boot, sizeof(boot) - 1);
    if (id >= 0) close(id);
    if (n <= 0) return 1;
    boot[n] = 0; boot[strcspn(boot, "\r\n")] = 0;
    snprintf(dir, sizeof(dir), "/data/superman-v70/fb-%s", boot);
    (void)mkdir("/data/superman-v70", 0700);
    if (mkdir(dir, 0700) != 0) return 1;
    snprintf(path, sizeof(path), "%s/state.txt", dir);
    out_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (out_fd < 0) return 1;
    static const unsigned times[] = {45, 90, 180, 540, 720, 780};
    emit("FB_START boot_id=%s pid=%ld", boot, (long)getpid());
    for (size_t attempt = 0; attempt < sizeof(times)/sizeof(times[0]); ++attempt) {
        struct timespec now;
        do {
            if (clock_gettime(CLOCK_BOOTTIME, &now)) return 1;
            if ((unsigned long)now.tv_sec >= times[attempt]) break;
            sleep(1);
        } while (1);
        emit("FB_BEGIN target=%u uptime=%lld", times[attempt], (long long)now.tv_sec);
        struct statvfs space;
        if (statvfs(dir, &space)) {
            emit("FB_SKIP statvfs path=%s errno=%d", dir, errno); continue;
        }
        uint64_t available = (uint64_t)space.f_bavail * space.f_frsize;
        emit("FB_SPACE available=%llu required=%u", (unsigned long long)available, 28*1024*1024);
        if (available < 28*1024*1024) {
            emit("FB_SKIP low_space"); continue;
        }
        int fb = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
        struct fb_fix_screeninfo fix;
        struct fb_var_screeninfo var;
        if (fb < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) || ioctl(fb, FBIOGET_VSCREENINFO, &var)) {
            emit("FB_ERROR open_or_ioctl errno=%d", errno);
            if (fb >= 0) close(fb);
            continue;
        }
        emit("FB_LAYOUT x=%u y=%u xv=%u yv=%u xo=%u yo=%u bpp=%u stride=%u smem_len=%u type=%u visual=%u",
             var.xres,var.yres,var.xres_virtual,var.yres_virtual,var.xoffset,var.yoffset,
             var.bits_per_pixel,fix.line_length,fix.smem_len,fix.type,fix.visual);
        emit("FB_CHANNELS r=%u:%u:%u g=%u:%u:%u b=%u:%u:%u a=%u:%u:%u",
             var.red.offset,var.red.length,var.red.msb_right,var.green.offset,var.green.length,var.green.msb_right,
             var.blue.offset,var.blue.length,var.blue.msb_right,var.transp.offset,var.transp.length,var.transp.msb_right);
        size_t remaining = fix.smem_len;
        if (remaining > 4*1024*1024) remaining = 4*1024*1024;
        snprintf(final, sizeof(final), "%s/%u.raw", dir, times[attempt]);
        snprintf(path, sizeof(path), "%s/%u.raw.incomplete", dir, times[attempt]);
        int dst = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        size_t total = 0;
        if (dst >= 0) {
            uint8_t buf[4096];
            while (remaining) {
                n = read(fb, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0 || write_all(dst, buf, (size_t)n)) break;
                total += (size_t)n; remaining -= (size_t)n;
            }
            int flushed = fsync(dst);
            close(dst);
            if (!remaining && !flushed && rename(path, final) == 0)
                emit("FB_SAVED bytes=%zu path=%s", total, final);
            else emit("FB_PARTIAL bytes=%zu remaining=%zu errno=%d", total, remaining, errno);
        } else emit("FB_ERROR output errno=%d", errno);
        close(fb);
    }
    emit("FB_END"); close(out_fd);
    if (kmsg_fd >= 0) close(kmsg_fd);
    return 0;
}

/* Separate from both framebuffer I/O and the shell collector. Only complete,
 * stable copies get a final name; retries and failed copies stay distinct. */
static int capture_tombstones(void) {
    char boot[80], dir[256], source[512], dest[512], final[512];
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, boot, sizeof(boot)-1);
    if (fd >= 0) close(fd);
    if (n <= 0) { emit("TS_ERROR boot_id errno=%d", errno); return 1; }
    boot[n]=0; boot[strcspn(boot,"\r\n")]=0;
    snprintf(dir,sizeof(dir),"/data/superman-v70/tombstones-%s-%ld",boot,(long)getpid());
    (void)mkdir("/data/superman-v70",0700);
    if (mkdir(dir,0700)) { emit("TS_ERROR mkdir errno=%d",errno); return 1; }
    snprintf(dest,sizeof(dest),"%s/state.txt",dir);
    out_fd=open(dest,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if (out_fd < 0) { emit("TS_ERROR output errno=%d",errno); return 1; }
    uint64_t written=0;
    unsigned sequence=0;
    emit("TS_START boot_id=%s",boot);
    for (unsigned poll=0; poll<600 && written<16*1024*1024; ++poll) {
        DIR *entries=opendir("/data/tombstones");
        if (!entries) { if (poll%30==0) emit("TS_ERROR opendir errno=%d",errno); sleep(2); continue; }
        struct dirent *entry;
        while ((entry=readdir(entries)) != NULL && written<16*1024*1024) {
            if (strncmp(entry->d_name,"tombstone_",10)) continue;
            snprintf(source,sizeof(source),"/data/tombstones/%s",entry->d_name);
            struct stat before,after;
            if (lstat(source,&before) || !S_ISREG(before.st_mode) || before.st_size<=0 || before.st_size>1024*1024) continue;
            off_t size=0; char hash[65], copied[65];
            if (hash_read(source,&size,hash)) continue;
            snprintf(final,sizeof(final),"%s/%s",dir,hash);
            if (access(final,F_OK)==0) continue;
            struct statvfs space;
            if (statvfs(dir,&space)) { emit("TS_ERROR statvfs errno=%d",errno); break; }
            if ((uint64_t)space.f_bavail*space.f_frsize < 8*1024*1024+(uint64_t)size) {
                emit("TS_SKIP low_space available=%llu",(unsigned long long)space.f_bavail*space.f_frsize); break;
            }
            if (written+(uint64_t)size>16*1024*1024) break;
            snprintf(dest,sizeof(dest),"%s/%u.incomplete",dir,++sequence);
            int in=open(source,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
            int out=open(dest,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            size_t total=0; int ok=in>=0 && out>=0;
            char buffer[4096];
            while (ok && total<(size_t)size) {
                size_t chunk=(size_t)size-total; if (chunk>sizeof(buffer)) chunk=sizeof(buffer);
                n=read(in,buffer,chunk);
                if (n<0 && errno==EINTR) continue;
                if (n<=0 || write_all(out,buffer,(size_t)n)) { ok=0; break; }
                total+=(size_t)n;
            }
            if (in>=0) close(in);
            if (out>=0) { if (fsync(out)) ok=0; close(out); }
            written+=total;
            if (stat(source,&after) || before.st_size!=after.st_size || before.st_mtime!=after.st_mtime) ok=0;
            if (hash_read(dest,&size,copied) || strcmp(hash,copied)) ok=0;
            if (ok && rename(dest,final)==0) {
                off_t verified_size = 0;
                char verified[65];
                int rc = hash_read(final, &verified_size, verified);
                int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                int sync_rc = dirfd < 0 ? -1 : fsync(dirfd);
                if (dirfd >= 0) close(dirfd);
                emit("TS_READBACK path=%s expected=%s actual=%s rc=%d size=%lld directory_sync=%d",
                     final, hash, rc == 0 ? verified : "unavailable", rc,
                     (long long)verified_size, sync_rc);
                if (rc == 0 && !strcmp(hash, verified) && verified_size == (off_t)total && sync_rc == 0)
                    emit("TS_SAVED source=%s sha256=%s bytes=%zu",source,hash,total);
                else emit("TS_VERIFY_FAILED preserved=%s", final);
            }
            else emit("TS_PARTIAL source=%s path=%s bytes=%zu",source,dest,total);
        }
        closedir(entries); sleep(2);
    }
    emit("TS_END bytes=%llu",(unsigned long long)written);
    close(out_fd); if (kmsg_fd>=0) close(kmsg_fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--tombstones") == 0) return capture_tombstones();
    if (argc == 2 && strcmp(argv[1], "--framebuffer") == 0) return capture_framebuffer();
    size_t i;
    open_output();
    int fault_setup = install_fault_capture();
    emit("FAULT_CAPTURE setup=%d errno=%d registers=ARM32-on-target", fault_setup,
         fault_setup < 0 ? errno : 0);
    capture_memory_map("/proc/iomem");
    capture_memory_map("/proc/meminfo");
    capture_memory_map("/proc/self/maps");
    capture_memory_map("/proc/device-tree/reserved-memory/diagnostic-quarantine@f500000/reg");
    emit("TEST read_then_mmap files=%zu", sizeof(targets) / sizeof(targets[0]));
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        fault_target = (sig_atomic_t)i;
        test_file(&targets[i]);
    }
    test_anonymous(0);
    test_cow();
    /* Runtime comparisons for r4 failures; no stale expected-build hashes. */
    static const struct target runtime_targets[] = {
        { "/system/bin/superman-memdiag", "unavailable" },
        /* r33: active ART compiler, once per existing bounded worker sweep.
         * Compare read/mmap digests offline with art_compiler_sha256 in this
         * boot's generated manifest. No baked-in r32 binary hash or extra
         * per-window SD writes. This is a fresh mapping, not the JIT's mapping.
         */
        { "/apex/com.android.art/lib/libart-compiler.so", "unavailable" },
        { "/system/framework/framework-res.apk", "unavailable" },
        { "/apex/com.android.runtime/lib/bionic/libdl.so", "unavailable" },
        { "/system/lib/libinput.so", "unavailable" },
        { "/system/lib/libcgrouprc.so", "unavailable" },
        { "/system/lib/libaudiopolicy.so", "unavailable" },
        { "/system/vendor/lib/egl/libGLESv2_swiftshader.so", "unavailable" },
    };
    static const unsigned sample_times[] = { 60, 75, 90, 120, 180 };
    for (size_t sample = 0; sample < sizeof(sample_times)/sizeof(sample_times[0]); ++sample) {
        struct timespec now = {0};
        while (clock_gettime(CLOCK_BOOTTIME, &now) == 0 &&
               now.tv_sec < (time_t)sample_times[sample]) sleep(1);
        emit("RUNTIME_SWEEP target=%u uptime=%lld", sample_times[sample], (long long)now.tv_sec);
        for (i = 0; i < sizeof(runtime_targets)/sizeof(runtime_targets[0]); ++i) {
            /* Isolate mapping faults and bound each diagnostic child. */
            fault_worker = 1; fault_target = (sig_atomic_t)i;
            fault_run = (sig_atomic_t)sample; fault_stage = 1;
            emit("WORKER_FORK kind=file run=%zu target=%zu path=%s", sample, i,
                 runtime_targets[i].path);
            pid_t child = fork();
            if (child == 0) {
                fault_stage = 2;
                emit("WORKER_ENTER kind=file pid=%ld", (long)getpid());
                alarm(180);
                if (!strcmp(runtime_targets[i].path, "/system/framework/framework-res.apk") ||
                    !strcmp(runtime_targets[i].path, "/system/lib/libinput.so"))
                    sample_file_bytes(runtime_targets[i].path);
                test_file(&runtime_targets[i]);
                fault_stage = 8;
                flush_log();
                _exit(0);
            }
            int status = 0;
            pid_t result = -1;
            if (child > 0) {
                do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            }
            emit("RUNTIME_RESULT path=%s child=%ld waited=%ld status=%d",
                 runtime_targets[i].path, (long)child, (long)result, status);
        }
        fault_worker = 2; fault_target = 0;
        fault_run = (sig_atomic_t)sample + 1; fault_stage = 1;
        emit("WORKER_FORK kind=anonymous run=%zu", sample + 1);
        pid_t memory_child = fork();
        if (memory_child == 0) {
            fault_stage = 2;
            emit("WORKER_ENTER kind=anonymous pid=%ld", (long)getpid());
            alarm(45);
            test_anonymous((unsigned)sample + 1);
            fault_stage = 8;
            flush_log();
            _exit(0);
        }
        int memory_status = 0;
        pid_t memory_result = -1;
        if (memory_child > 0) {
            do { memory_result = waitpid(memory_child, &memory_status, 0); }
            while (memory_result < 0 && errno == EINTR);
        }
        emit("ANON_WORKER run=%u child=%ld waited=%ld status=%d",
             (unsigned)sample + 1, (long)memory_child,
             (long)memory_result, memory_status);
        /* Bracket a forked test with identical parent tests. Times are recorded;
         * these are sequential controls, not simultaneous identical conditions.
         */
        fault_worker = 0; fault_run = (sig_atomic_t)sample + 1;
        test_page_sequence((unsigned)sample + 1, "parent-before");
        fault_worker = 3; fault_stage = 1;
        pid_t page_child = fork();
        if (page_child == 0) {
            alarm(45);
            test_page_sequence((unsigned)sample + 1, "child");
            flush_log();
            _exit(0);
        }
        int page_status = 0;
        pid_t page_wait = -1;
        if (page_child > 0)
            do { page_wait = waitpid(page_child, &page_status, 0); }
            while (page_wait < 0 && errno == EINTR);
        emit("PAGE_WORKER run=%zu child=%ld waited=%ld status=%d", sample + 1,
             (long)page_child, (long)page_wait, page_status);
        fault_worker = 0;
        test_page_sequence((unsigned)sample + 1, "parent-after");
    }
    emit("END completed=yes; inspect individual file, anonymous and COW results");
    if (out_fd >= 0) { fsync(out_fd); close(out_fd); }
    if (kmsg_fd >= 0) close(kmsg_fd);
    return 0;
}

/* Host-only test: syscalls and pidfd polling are mocked; no process is signalled.
 * argv[1] is the extracted, hash-verified r37 ramdisk init ELF. */
#include <assert.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
static int unsupported, dead, denied, sent, bound;
static long fake_syscall(long op, ...) {
    va_list ap;
    va_start(ap, op);
    if (op == __NR_pidfd_open) {
        long pid = va_arg(ap, long);
        assert(pid == 1234);
        va_end(ap);
        if (unsupported) { errno = ENOSYS; return -1; }
        bound = open("/dev/null", O_RDONLY);
        return bound;
    }
    assert(op == __NR_pidfd_send_signal);
    int fd = va_arg(ap, int), sig = va_arg(ap, int);
    assert(fd == bound && sig == SIGSTOP);
    va_end(ap);
    ++sent;
    if (denied) { errno = EPERM; return -1; }
    return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 1 && timeout == 0 && fds[0].fd == bound);
    return dead;
}
#define syscall fake_syscall
#define poll fake_poll
#define main collector_main
#include "../superman-kerneldiag.c"
#undef main
#undef poll
#undef syscall
static void text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    char root[] = "/tmp/superman-handoff-XXXXXX";
    assert(mkdtemp(root));
    char proc[256], comm[300], exe[300], pid1[256];
    snprintf(proc, sizeof(proc), "%s/1234", root);
    snprintf(pid1, sizeof(pid1), "%s/1", root);
    assert(!mkdir(proc, 0700));
    snprintf(comm, sizeof(comm), "%s/comm", proc);
    snprintf(exe, sizeof(exe), "%s/exe", proc);
    text_file(comm, "lumia-a11-log\n");
    assert(!symlink(argv[1], exe));
    int handle = pause_legacy_logger(root);
    assert(handle >= 0 && sent == 1); close(handle);
    sent = 0; text_file(comm, "init\n");
    assert(pause_legacy_logger(root) < 0 && !sent);
    text_file(comm, "lumia-a11-log\n");
    unsupported = 1; assert(pause_legacy_logger(root) < 0 && !sent); unsupported = 0;
    dead = 1; assert(pause_legacy_logger(root) < 0 && !sent); dead = 0;
    denied = 1; assert(pause_legacy_logger(root) < 0 && sent == 1); denied = 0; sent = 0;
    assert(!rename(proc, pid1));
    assert(pause_legacy_logger(root) < 0 && !sent);
    assert(!rename(pid1, proc));
    assert(!unlink(exe));
    /* Same-sized impostor must fail the executable hash gate. */
    int fake = open(exe, O_CREAT | O_WRONLY, 0600); assert(fake >= 0);
    assert(!ftruncate(fake, 463984)); close(fake);
    assert(pause_legacy_logger(root) < 0 && !sent);
    assert(!unlink(exe)); assert(!unlink(comm)); assert(!rmdir(proc)); assert(!rmdir(root));
    puts("PASS: exact wrapper, name/hash/PID1 rejection, pidfd unsupported/dead, signal failure");
    return 0;
}

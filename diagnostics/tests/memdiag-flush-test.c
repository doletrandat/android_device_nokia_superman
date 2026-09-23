/* Host-only: replace time/fsync; never execute the memory or framebuffer tests. */
#define _LARGEFILE64_SOURCE
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
static time_t test_time = 100;
static unsigned flushes;
static int test_sync(int fd) { assert(fd >= 0); ++flushes; return 0; }
static int test_clock(clockid_t id, struct timespec *ts) {
    (void)id; ts->tv_sec = test_time; ts->tv_nsec = 0; return 0;
}
#define fsync test_sync
#define clock_gettime test_clock
#define main memdiag_main
#include "../superman-memdiag.c"
#undef main
#undef clock_gettime
#undef fsync
int main(void) {
    FILE *file = tmpfile(); assert(file);
    out_fd = fileno(file);
    emit("ordinary first"); assert(flushes == 1);
    test_time = 101;
    for (int i = 0; i < 100; ++i) emit("ordinary burst %d", i);
    assert(flushes == 1);
    test_time = 130; emit("ordinary due"); assert(flushes == 2);
    emit("PAGE_FAIL detail"); assert(flushes == 3);
    emit("ANON_SNAPSHOT detail"); assert(flushes == 4);
    emit("FILE_READ rc=-5"); assert(flushes == 5);
    emit("END completed=yes"); assert(flushes == 6);
    flush_log(); assert(flushes == 7);
    assert(!fclose(file));
    puts("PASS: burst batching, 30s cadence, failures, snapshot and completion flush");
    return 0;
}

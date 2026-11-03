// SPDX-License-Identifier: Apache-2.0
// Three fresh processes; private pipes gate finite work and idle intervals.
#pragma once
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cycle {
constexpr int kWorkers = 3;
static volatile sig_atomic_t children[kWorkers]{};
static int notify = -1, gate = -1, worker = -1;
inline int number(const char* s) {
    char* end = nullptr; errno = 0; long v = std::strtol(s, &end, 10);
    return errno || !*s || *end || v < 0 || v > INT_MAX ? -1 : static_cast<int>(v);
}
inline bool init_worker(const char* readFd, const char* writeFd, const char* id) {
    gate = number(readFd); notify = number(writeFd); worker = number(id);
    struct stat a{}, b{};
    return gate >= 3 && gate < 512 && notify >= 3 && notify < 512 && gate != notify &&
        worker >= 0 && worker < kWorkers && !fstat(gate, &a) && !fstat(notify, &b) &&
        S_ISFIFO(a.st_mode) && S_ISFIFO(b.st_mode);
}
inline bool receive(int fd, char expected) {
    pollfd p{fd, POLLIN, 0}; int ret;
    do { ret = poll(&p, 1, 12000); } while (ret < 0 && errno == EINTR);
    char value = 0;
    return ret > 0 && (p.revents & POLLIN) && read(fd, &value, 1) == 1 && value == expected;
}
inline bool barrier(char marker) {
    return write(notify, &marker, 1) == 1 && receive(gate, 'G');
}
inline void timeout(int) {
    for (int i = 0; i < kWorkers; ++i) if (children[i] > 0) kill(children[i], SIGKILL);
    constexpr char msg[] = "GPU_CYCLE coordinator_watchdog; no PASS\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg)-1); _exit(124);
}
inline void cached(const char* phase, int round) {
    constexpr const char* files[] = {"runtime_status", "runtime_suspended_time", "runtime_active_time"};
    for (const char* file : files) {
        char path[192], value[128]{};
        std::snprintf(path, sizeof(path), "/sys/bus/platform/devices/fdb00000.gpu/power/%s", file);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        const ssize_t n = fd < 0 ? -1 : read(fd, value, sizeof(value)-1);
        if (fd >= 0) close(fd);
        if (n > 0) value[std::strcspn(value, "\r\n")] = 0;
        std::printf("GPU_CYCLE cached phase=%s round=%d field=%s value=%s MMIO_reads=0\n",
            phase, round, file, n > 0 ? value : "unavailable");
    }
}
inline void clients() {
    DIR* dir = opendir("/proc/self/fd"); if (!dir) return;
    int count = 0;
    while (dirent* e = readdir(dir)) {
        int fd = number(e->d_name); if (fd < 0) continue;
        char path[128], target[256]{};
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, target, sizeof(target)-1);
        if (n <= 0 || std::strncmp(target, "/dev/dri/", 9)) continue;
        if (++count > 8) break;
        char info[4096]{}; std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        n = f < 0 ? -1 : read(f, info, sizeof(info)-1); if (f >= 0) close(f);
        std::printf("GPU_CYCLE client worker=%d pid=%d fd=%d target=%s\n", worker, getpid(), fd, target);
        if (n > 0) {
            for (char* p = std::strtok(info, "\n"); p; p = std::strtok(nullptr, "\n"))
                if (!std::strncmp(p, "drm-", 4))
                    std::printf("GPU_CYCLE client-info worker=%d %s\n", worker, p);
        }
    }
    closedir(dir);
}
inline int coordinator(char** argv) {
    int ready[kWorkers][2], go[kWorkers][2];
    for (int i = 0; i < kWorkers; ++i)
        if (pipe2(ready[i], O_CLOEXEC) || pipe2(go[i], O_CLOEXEC)) return 7;
    struct sigaction sa{}; sa.sa_handler = timeout; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, nullptr)) return 7;
    signal(SIGPIPE, SIG_IGN); alarm(45);
    bool okay = true;
    for (int i = 0; i < kWorkers; ++i) {
        pid_t pid = fork();
        if (pid < 0) { okay = false; break; }
        if (!pid) {
            for (int j = 0; j < kWorkers; ++j) {
                close(ready[j][0]); close(go[j][1]);
                if (j != i) { close(ready[j][1]); close(go[j][0]); }
            }
            if (fcntl(ready[i][1], F_SETFD, 0) || fcntl(go[i][0], F_SETFD, 0)) _exit(7);
            char r[16], w[16], id[16];
            std::snprintf(r, sizeof(r), "%d", go[i][0]); std::snprintf(w, sizeof(w), "%d", ready[i][1]);
            std::snprintf(id, sizeof(id), "%d", i);
            char* args[] = {argv[0], const_cast<char*>("--worker"), argv[2], argv[3], argv[4],
                const_cast<char*>("--ipc"), r, w, const_cast<char*>("--id"), id, nullptr};
            execv("/proc/self/exe", args); _exit(7);
        }
        children[i] = pid;
        std::printf("GPU_CYCLE worker-spawn worker=%d pid=%d\n", i, pid);
    }
    for (int i = 0; i < kWorkers; ++i) { close(ready[i][1]); close(go[i][0]); }
    auto collect = [&](char marker) {
        for (int i = 0; i < kWorkers; ++i) if (!receive(ready[i][0], marker)) return false;
        return true;
    };
    auto release = [&]() {
        for (int i = 0; i < kWorkers; ++i) if (write(go[i][1], "G", 1) != 1) return false;
        return true;
    };
    okay = okay && collect('R') && release();
    const int rounds = !std::strcmp(argv[4], "full") ? 2 : 6;
    for (int round = 0; okay && round < rounds; ++round) {
        okay = collect('I'); if (!okay) break;
        cached("idle_begin", round);
        // All private workers have completed their readbacks and are blocked.
        // Android remains active: suspension must be observed, never assumed.
        if (usleep(250000)) { okay = false; break; }
        cached("idle_end", round); okay = release();
    }
    if (!okay) for (int i = 0; i < kWorkers; ++i) if (children[i] > 0) kill(children[i], SIGKILL);
    for (int i = 0; i < kWorkers; ++i) {
        if (children[i] > 0) {
            int status = 0; pid_t got;
            do { got = waitpid(children[i], &status, 0); } while (got < 0 && errno == EINTR);
            children[i] = 0;
            if (got < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) okay = false;
            std::printf("GPU_CYCLE worker-exit worker=%d wait_status=%d\n", i, status);
        }
        close(ready[i][0]); close(go[i][1]);
    }
    alarm(0);
    std::printf("GPU_CYCLE COORDINATOR_%s workers=3 mode=%s size=%s rounds=%d idle_ms=250 not_fps=1\n",
        okay ? "PASS" : "FAIL", argv[2], argv[4], rounds);
    return okay ? 0 : 7;
}
} // namespace cycle

// SPDX-License-Identifier: Apache-2.0
// Lumia 730/MSM8226 inherited display inventory. Standalone, never a HAL.
// Read-only mappings and an immutable allowlist: no reset, ACK, clock, DMA,
// framebuffer, interrupt-enable, counter-enable or panel command writes.
// Layout: mdp5_cfg.c msm8x26; registers/display/{mdp5,dsi}.xml; DSI6G +4.
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {
struct Register { const char* name; uint32_t address; };
constexpr Register registers[] = {
    {"MDP_HW_REV", 0xfd900100}, {"MDP_INTF_SEL", 0xfd900104},
    {"CTL1_LAYER0", 0xfd900600}, {"CTL1_TOP", 0xfd900614},
    {"RGB0_SRC_SIZE", 0xfd901e00}, {"RGB0_IMG_SIZE", 0xfd901e04},
    {"RGB0_SRC_XY", 0xfd901e08}, {"RGB0_OUT_SIZE", 0xfd901e0c},
    {"RGB0_OUT_XY", 0xfd901e10}, {"RGB0_SRC0_ADDR", 0xfd901e14},
    {"RGB0_STRIDE", 0xfd901e24}, {"RGB0_FORMAT", 0xfd901e30},
    {"RGB0_UNPACK", 0xfd901e34}, {"RGB0_OP_MODE", 0xfd901e38},
    {"INTF1_ENABLE", 0xfd921300}, {"INTF1_CONFIG", 0xfd921304},
    {"INTF1_HSYNC_CTL", 0xfd921308}, {"INTF1_VSYNC_PERIOD_F0", 0xfd92130c},
    {"INTF1_VSYNC_LEN_F0", 0xfd921314}, {"INTF1_DISPLAY_VSTART_F0", 0xfd92131c},
    {"INTF1_DISPLAY_VEND_F0", 0xfd921324}, {"INTF1_ACTIVE_VSTART_F0", 0xfd92132c},
    {"INTF1_ACTIVE_VEND_F0", 0xfd921334}, {"INTF1_DISPLAY_HCTL", 0xfd92133c},
    {"INTF1_ACTIVE_HCTL", 0xfd921340}, {"INTF1_HSYNC_SKEW", 0xfd92134c},
    {"INTF1_POLARITY_CTL", 0xfd921350}, {"INTF1_PANEL_FORMAT", 0xfd921390},
    {"INTF1_COUNT_ENABLE", 0xfd9213a8}, {"INTF1_FRAME_COUNT", 0xfd9213ac},
    {"INTF1_LINE_COUNT", 0xfd9213b0},
    {"DSI0_HW_VERSION", 0xfd922800}, {"DSI0_CTRL", 0xfd922804},
    {"DSI0_VIDEO_CFG0", 0xfd922810}, {"DSI0_VIDEO_CFG1", 0xfd922820},
    {"DSI0_ACTIVE_H", 0xfd922824}, {"DSI0_ACTIVE_V", 0xfd922828},
    {"DSI0_TOTAL", 0xfd92282c}, {"DSI0_ACTIVE_HSYNC", 0xfd922830},
    {"DSI0_VSYNC_HPOS", 0xfd922834}, {"DSI0_VSYNC_VPOS", 0xfd922838},
    {"DSI0_LANE_SWAP", 0xfd9228b0}, {"DSI0_VERSION", 0xfd9229f4},
};
constexpr uint32_t pages[] = {0xfd900000, 0xfd901000, 0xfd921000, 0xfd922000};
struct Maps {
    void* base[4] = {MAP_FAILED, MAP_FAILED, MAP_FAILED, MAP_FAILED};
    ~Maps() { for (void* p : base) if (p != MAP_FAILED) munmap(p, 4096); }
    uint32_t read(uint32_t address) const {
        for (unsigned i = 0; i < 4; ++i)
            if ((address & ~uint32_t(4095)) == pages[i])
                return *reinterpret_cast<const volatile uint32_t*>(
                    static_cast<const char*>(base[i]) + (address & 4095));
        // No caller-supplied address exists. Fail closed if the allowlist drifts.
        _exit(3);
    }
};
uint64_t now() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(3);
    return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--query-only")) return 2;
    alarm(8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (getpagesize() != 4096) return 3;
    char compatible[128]{};
    int board = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    ssize_t length = board < 0 ? -1 : read(board, compatible, sizeof(compatible));
    if (board >= 0) close(board);
    constexpr char expected[] = "nokia,superman\0qcom,msm8226\0";
    if (length != sizeof(expected) - 1 ||
        std::memcmp(compatible, expected, sizeof(expected) - 1)) {
        std::puts("HANDOFF unsupported_board=true mmio_read=false"); return 3;
    }
    int fb = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    fb_fix_screeninfo fix{}; fb_var_screeninfo var{};
    if (fb < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) ||
        ioctl(fb, FBIOGET_VSCREENINFO, &var)) {
        std::printf("HANDOFF fb_query_failed errno=%d\n", errno);
        if (fb >= 0) close(fb);
        return 1;
    }
    close(fb);
    if (std::strncmp(fix.id, "simple", sizeof(fix.id)) ||
        fix.smem_start != 0x03200000 || fix.smem_len != 0x00800000 ||
        var.xres != 720 || var.yres != 1280 || var.bits_per_pixel != 32 ||
        fix.line_length != 2880 || var.xoffset || var.yoffset) {
        std::puts("HANDOFF unsupported_fb=true mmio_read=false"); return 3;
    }
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) { std::printf("HANDOFF mem_open_failed errno=%d\n", errno); return 1; }
    Maps maps;
    // First map/check only the known MDP revision and interface selection.
    // Addresses above 2GiB require mmap64 on Android's ARM32 off_t ABI.
    maps.base[0] = mmap64(nullptr, 4096, PROT_READ, MAP_SHARED, fd, off64_t(pages[0]));
    if (maps.base[0] == MAP_FAILED) {
        std::printf("HANDOFF map_failed page=0x%08x errno=%d\n", pages[0], errno);
        close(fd); return 1;
    }
    if (maps.read(0xfd900100) != 0x10010001 || maps.read(0xfd900104) != 0x100) {
        std::puts("HANDOFF unsupported_mdp=true"); close(fd); return 3;
    }
    for (unsigned i = 1; i < 4; ++i) {
        maps.base[i] = mmap64(nullptr, 4096, PROT_READ, MAP_SHARED, fd, off64_t(pages[i]));
        if (maps.base[i] == MAP_FAILED) {
            std::printf("HANDOFF map_failed page=0x%08x errno=%d\n", pages[i], errno);
            close(fd); return 1;
        }
    }
    close(fd);
    if (maps.read(0xfd921300) != 1 || maps.read(0xfd901e14) != fix.smem_start ||
        maps.read(0xfd901e00) != 0x050002d0 || maps.read(0xfd901e24) != 2880) {
        std::puts("HANDOFF unsupported_active_scanout=true"); return 3;
    }
    for (const auto& r : registers)
        std::printf("HANDOFF %s address=0x%08x value=0x%08x\n",
                    r.name, r.address, maps.read(r.address));
    const uint32_t enabled = maps.read(0xfd9213a8);
    const uint64_t t0 = now();
    const uint32_t frame0 = maps.read(0xfd9213ac), line0 = maps.read(0xfd9213b0);
    // Never enable a counter or ACK an IRQ to obtain a measurement.
    unsigned changes = 0, samples = 0;
    uint32_t previous = line0, lastFrame = frame0;
    // This revision can expose running counters while COUNT_ENABLE reads zero.
    // Observe whether they advance; do not infer activity from that bit alone.
    const timespec delay{0, 2000000};
    do {
        nanosleep(&delay, nullptr);
        uint32_t line = maps.read(0xfd9213b0);
        changes += line != previous; previous = line;
        lastFrame = maps.read(0xfd9213ac); ++samples;
    } while (now() - t0 < 2000000000ULL);
    const uint64_t dt = now() - t0;
    std::printf("HANDOFF counters enable=0x%x elapsed_ns=%" PRIu64
                " first_frame=%u last_frame=%u samples=%u line_changes=%u\n",
                enabled, dt, frame0, lastFrame, samples, changes);
    std::puts("HANDOFF query_complete=true mmio_write=false framebuffer_write=false"
              " clock_change=false irq_ack=false modeset=false");
    return 0;
}

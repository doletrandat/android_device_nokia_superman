// SPDX-License-Identifier: Apache-2.0
// Queries only: no DRM master acquisition, BO allocation, MMIO, modeset or post.
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cinttypes>
int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--query-only")) return 2;
    alarm(5);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int fb = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fb < 0) { std::printf("DISPLAY fb_open_errno=%d\n", errno); return 1; }
    fb_fix_screeninfo fix{}; fb_var_screeninfo var{}; fb_vblank blank{};
    if (ioctl(fb, FBIOGET_FSCREENINFO, &fix) || ioctl(fb, FBIOGET_VSCREENINFO, &var)) {
        std::printf("DISPLAY fb_query_errno=%d\n", errno); close(fb); return 1;
    }
    std::printf("DISPLAY fb=%.*s size=%ux%u virtual=%ux%u offset=%u,%u bpp=%u stride=%u smem_len=%u physical=0x%lx pixclock_ps=%u margins=%u,%u,%u,%u sync=%u,%u\n",
        int(sizeof(fix.id)), fix.id, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
        var.xoffset, var.yoffset, var.bits_per_pixel, fix.line_length, fix.smem_len,
        fix.smem_start, var.pixclock, var.left_margin, var.right_margin,
        var.upper_margin, var.lower_margin, var.hsync_len, var.vsync_len);
    errno = 0; int rc = ioctl(fb, FBIOGET_VBLANK, &blank); int error = rc ? errno : 0;
    std::printf("DISPLAY fb_get_vblank_rc=%d errno=%d flags=0x%x count=%u vcount=%u hcount=%u\n",rc,error,blank.flags,blank.count,blank.vcount,blank.hcount);
    close(fb);
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { std::printf("DISPLAY drm_open_errno=%d\n", errno); return 1; }
    drmVersionPtr version = drmGetVersion(fd);
    if (version) {
        std::printf("DISPLAY drm_driver=%.*s version=%d.%d.%d\n",version->name_len,version->name,version->version_major,version->version_minor,version->version_patchlevel);
        drmFreeVersion(version);
    }
    uint64_t dumb=0;
    rc=drmGetCap(fd,DRM_CAP_DUMB_BUFFER,&dumb);
    std::printf("DISPLAY drm_dumb_rc=%d value=%" PRIu64 "\n",rc,dumb);
    errno=0; drmModeResPtr res=drmModeGetResources(fd);error=res?0:errno;
    if(res) {
        std::printf("DISPLAY drm_resources crtcs=%d connectors=%d encoders=%d framebuffers=%d max_size=%ux%u\n",res->count_crtcs,res->count_connectors,res->count_encoders,res->count_fbs,res->max_width,res->max_height);
        drmModeFreeResources(res);
    } else std::printf("DISPLAY drm_resources_unavailable_errno=%d\n",error);
    close(fd);
    std::puts("DISPLAY query_complete=true framebuffer_write=false modeset=false master_acquisition=false");
    return 0;
}

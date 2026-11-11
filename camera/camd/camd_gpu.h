#pragma once
#include <stdint.h>

/* GPU ISP (see camd_gpu.c). All calls from one thread. */
extern unsigned gpu_phase_x, gpu_phase_y; /* CFA phase: position of R (set before gpu_init) */
extern unsigned gpu_raw_w;                /* image width in pixels (0: bytesperline * 4 / 5) */
extern int gpu_black_g, gpu_black_rb;     /* 10-bit black levels for the general path */
extern int gpu_defect_col;                /* first of a weak column pair (general path), -1: none */
int gpu_init(unsigned bytesperline, unsigned height, unsigned out_w, unsigned out_h);
int gpu_import(unsigned idx, int dmabuf_fd);
void gpu_release_imports(void);
/* idx: an imported capture buffer, or -1 to upload `raw` */
int gpu_process(int idx, const uint8_t *raw, unsigned ox, unsigned oy, const float wb[3], float sat,
		const float ccm[9], int out_idx, uint8_t *out_yuyv);
/* render targets: the bridge's capture buffers (out_idx >= 0 renders into one of them) */
int gpu_out_import(unsigned idx, int dmabuf_fd);
void gpu_out_release(void);
void gpu_fini(void);

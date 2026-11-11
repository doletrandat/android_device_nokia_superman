/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* superman-camfeed feed-device ioctls (zero-copy producer path). */
#ifndef SUPERMAN_CAMFEED_H
#define SUPERMAN_CAMFEED_H
#include <linux/ioctl.h>
#include <linux/types.h>

struct camfeed_expbuf {
	__u32 index;  /* in: capture buffer index */
	__s32 fd;     /* out: dma-buf fd (O_RDWR | O_CLOEXEC) */
	__u32 count;  /* out: number of capture buffers */
	__u32 size;   /* out: plane size in bytes */
};

struct camfeed_buf {
	__u32 index;      /* ACQUIRE out / DONE in */
	__u32 timeout_ms; /* ACQUIRE in */
	__u32 ok;         /* DONE in: 1 = frame written, 0 = return with an error */
	__u32 reserved;
};

struct camfeed_fmt {
	__u32 width, height; /* out: the capture format the consumer chose (YUYV) */
};

/* Current capture size (the producer renders frames of exactly this size). */
#define CAMFEED_IOC_GETFMT  _IOR('C', 4, struct camfeed_fmt)
/* Export capture buffer `index` as a dma-buf (only while the consumer has buffers). */
#define CAMFEED_IOC_EXPBUF  _IOWR('C', 1, struct camfeed_expbuf)
/* Take the oldest empty queued buffer; -EAGAIN on timeout, -ENODATA when not streaming. */
#define CAMFEED_IOC_ACQUIRE _IOWR('C', 2, struct camfeed_buf)
/* Hand an acquired buffer to the consumer (or back with an error). */
#define CAMFEED_IOC_DONE    _IOW('C', 3, struct camfeed_buf)
#endif

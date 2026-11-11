// SPDX-License-Identifier: GPL-2.0
/*
 * superman-camfeed: fixed-format V4L2 capture devices fed from userspace.
 *
 * The Lumia 730 cameras produce Bayer RAW10 through CAMSS; superman-camd converts it (GPU) to YUYV
 * and hands each frame to one of these devices, whose capture node the Android external camera
 * provider reads like a webcam. Device i: /dev/video<video_nr + i> ("superman-rear" /
 * "superman-front"), fed through /dev/superman-camfeed / /dev/superman-camfeed1.
 *
 * Feed device protocol:
 *   write(fd, frame, width * height * 2)  completes the oldest queued capture buffer (dropped if
 *                                         no buffer is queued); a short write is -EINVAL.
 *   read(fd, &c, 1)                       blocks until the capture streaming state changed since
 *                                         the last read on this fd; c = '0' or '1' (the current
 *                                         state: a stop+start pair reads as '1' again).
 *   poll                                  POLLIN when such a change is pending, POLLOUT always.
 *   ioctls (superman_camfeed.h)           size query, and zero-copy: export the capture buffers
 *                                         as dma-bufs, then ACQUIRE an empty one, render, DONE.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <asm/cacheflush.h>

#include "superman_camfeed.h"

#define MAX_DEV 2
static unsigned int devices = 1, fps = 15;
static unsigned int width[MAX_DEV] = { 1408, 1280 }, height[MAX_DEV] = { 1056, 960 };
static int video_nr = 10; /* first node; device i is video_nr + i */
module_param(devices, uint, 0444);
module_param_array(width, uint, NULL, 0444);
module_param_array(height, uint, NULL, 0444);
module_param(fps, uint, 0444);
module_param(video_nr, int, 0444);
static const char *const dev_names[MAX_DEV] = { "superman-rear", "superman-front" };
static const char *const feed_names[MAX_DEV] = { "superman-camfeed", "superman-camfeed1" };

/* Sizes offered: the sensor window (width/height params) first, then smaller standard sizes. */
struct cf_size { u32 w, h; };
static const struct cf_size std_sizes[] = {
	/* unbinned stills (camd reads the sensor out at full resolution above its binned window) */
	{ 2560, 1920 }, { 2048, 1536 }, { 1600, 1200 }, { 1408, 1056 },
	{ 1280, 960 }, { 1280, 720 }, { 960, 720 }, { 720, 480 },
	{ 640, 480 }, { 352, 288 }, { 320, 240 }, { 176, 144 },
};

struct cf_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct camfeed {
	struct v4l2_device v4l2;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;      /* ioctl serialisation */
	spinlock_t qlock;       /* queued list */
	struct mutex feed_lock; /* a frame write vs. stream stop */
	struct list_head queued;
	struct miscdevice misc;
	wait_queue_head_t state_wq;
	atomic_t state_gen;     /* bumped on every streaming change */
	bool streaming;
	struct cf_buf *held[VB2_MAX_FRAME]; /* acquired by the producer */
	wait_queue_head_t buf_wq;           /* a buffer was queued */
	u32 sequence;
	unsigned int frame_size;
	u32 cur_w, cur_h;
	struct cf_size sizes[ARRAY_SIZE(std_sizes) + 1];
	unsigned int n_sizes;
	const char *card;
};

static struct camfeed *cfs[MAX_DEV];

static unsigned int cf_slack(struct camfeed *cf)
{
	/* a GPU rendering into the buffer may write whole 32-row tiles past the frame */
	return PAGE_ALIGN(cf->frame_size + cf->cur_w * 2 * 32 + SZ_64K);
}

/* --------------------------------------------------------------- vb2 */
static int cf_queue_setup(struct vb2_queue *q, unsigned int *nbuf, unsigned int *nplanes,
			  unsigned int sizes[], struct device *alloc_devs[])
{
	struct camfeed *cf = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < cf_slack(cf) ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = cf_slack(cf);
	if (*nbuf < 2)
		*nbuf = 2;
	return 0;
}

static int cf_buf_prepare(struct vb2_buffer *vb)
{
	struct camfeed *cf = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < cf->frame_size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, cf->frame_size);
	return 0;
}

static void cf_buf_queue(struct vb2_buffer *vb)
{
	struct camfeed *cf = vb2_get_drv_priv(vb->vb2_queue);
	struct cf_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct cf_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&cf->qlock, flags);
	list_add_tail(&b->list, &cf->queued);
	spin_unlock_irqrestore(&cf->qlock, flags);
	wake_up_interruptible(&cf->buf_wq);
}

static void cf_set_streaming(struct camfeed *cf, bool on)
{
	cf->streaming = on;
	atomic_inc(&cf->state_gen);
	wake_up_interruptible(&cf->state_wq);
}

static void cf_return_all(struct camfeed *cf, enum vb2_buffer_state state)
{
	struct cf_buf *b, *n;
	unsigned long flags;

	spin_lock_irqsave(&cf->qlock, flags);
	list_for_each_entry_safe(b, n, &cf->queued, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&cf->qlock, flags);
}

static int cf_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct camfeed *cf = vb2_get_drv_priv(q);

	cf->sequence = 0;
	cf_set_streaming(cf, true);
	return 0;
}

static void cf_stop_streaming(struct vb2_queue *q)
{
	struct camfeed *cf = vb2_get_drv_priv(q);

	cf_set_streaming(cf, false);
	mutex_lock(&cf->feed_lock); /* a write in flight completes its buffer first */
	cf_return_all(cf, VB2_BUF_STATE_ERROR);
	for (int i = 0; i < VB2_MAX_FRAME; i++)
		if (cf->held[i]) {
			vb2_buffer_done(&cf->held[i]->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			cf->held[i] = NULL;
		}
	mutex_unlock(&cf->feed_lock);
	wake_up_interruptible(&cf->buf_wq);
}

static const struct vb2_ops cf_qops = {
	.queue_setup = cf_queue_setup,
	.buf_prepare = cf_buf_prepare,
	.buf_queue = cf_buf_queue,
	.start_streaming = cf_start_streaming,
	.stop_streaming = cf_stop_streaming,
};

/* --------------------------------------------------------------- V4L2 ioctls */
static void cf_fill_pix(struct v4l2_pix_format *p, u32 w, u32 h)
{
	p->width = w;
	p->height = h;
	p->pixelformat = V4L2_PIX_FMT_YUYV;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = w * 2;
	p->sizeimage = w * h * 2;
	p->colorspace = V4L2_COLORSPACE_JPEG;
	p->ycbcr_enc = V4L2_YCBCR_ENC_601;
	p->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	p->xfer_func = V4L2_XFER_FUNC_SRGB;
}

/* exact size if offered, else the smallest offered size that covers the request, else the largest */
static unsigned int cf_pick(struct camfeed *cf, u32 w, u32 h)
{
	int best = -1;

	for (unsigned int i = 0; i < cf->n_sizes; i++) {
		if (cf->sizes[i].w == w && cf->sizes[i].h == h)
			return i;
		if (cf->sizes[i].w >= w && cf->sizes[i].h >= h &&
		    (best < 0 || cf->sizes[i].w * cf->sizes[i].h < cf->sizes[best].w * cf->sizes[best].h))
			best = i;
	}
	return best < 0 ? 0 : best;
}

static int cf_querycap(struct file *f, void *p, struct v4l2_capability *cap)
{
	struct camfeed *cf = video_drvdata(f);

	strscpy(cap->driver, "superman-camfeed", sizeof(cap->driver));
	strscpy(cap->card, cf->card, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", cf->card);
	return 0;
}

static int cf_enum_fmt(struct file *f, void *p, struct v4l2_fmtdesc *d)
{
	if (d->index)
		return -EINVAL;
	d->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int cf_g_fmt(struct file *f, void *p, struct v4l2_format *fmt)
{
	struct camfeed *cf = video_drvdata(f);

	cf_fill_pix(&fmt->fmt.pix, cf->cur_w, cf->cur_h);
	return 0;
}

static int cf_try_fmt(struct file *f, void *p, struct v4l2_format *fmt)
{
	struct camfeed *cf = video_drvdata(f);
	unsigned int i = cf_pick(cf, fmt->fmt.pix.width, fmt->fmt.pix.height);

	cf_fill_pix(&fmt->fmt.pix, cf->sizes[i].w, cf->sizes[i].h);
	return 0;
}

static int cf_s_fmt(struct file *f, void *p, struct v4l2_format *fmt)
{
	struct camfeed *cf = video_drvdata(f);
	unsigned int i = cf_pick(cf, fmt->fmt.pix.width, fmt->fmt.pix.height);

	if (vb2_is_busy(&cf->queue) && (cf->sizes[i].w != cf->cur_w || cf->sizes[i].h != cf->cur_h))
		return -EBUSY;
	cf->cur_w = cf->sizes[i].w;
	cf->cur_h = cf->sizes[i].h;
	cf->frame_size = cf->cur_w * cf->cur_h * 2;
	cf_fill_pix(&fmt->fmt.pix, cf->cur_w, cf->cur_h);
	return 0;
}

static int cf_enum_framesizes(struct file *f, void *p, struct v4l2_frmsizeenum *fs)
{
	struct camfeed *cf = video_drvdata(f);

	if (fs->index >= cf->n_sizes || fs->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = cf->sizes[fs->index].w;
	fs->discrete.height = cf->sizes[fs->index].h;
	return 0;
}

static int cf_enum_frameintervals(struct file *f, void *p, struct v4l2_frmivalenum *fi)
{
	struct camfeed *cf = video_drvdata(f);
	unsigned int i = cf_pick(cf, fi->width, fi->height);

	if (fi->index || fi->pixel_format != V4L2_PIX_FMT_YUYV || cf->sizes[i].w != fi->width ||
	    cf->sizes[i].h != fi->height)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = fps;
	return 0;
}

static int cf_g_parm(struct file *f, void *p, struct v4l2_streamparm *sp)
{
	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.timeperframe.numerator = 1;
	sp->parm.capture.timeperframe.denominator = fps;
	sp->parm.capture.readbuffers = 2;
	return 0;
}

static int cf_enum_input(struct file *f, void *p, struct v4l2_input *in)
{
	if (in->index)
		return -EINVAL;
	strscpy(in->name, "Camera", sizeof(in->name));
	in->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int cf_g_input(struct file *f, void *p, unsigned int *i) { *i = 0; return 0; }
static int cf_s_input(struct file *f, void *p, unsigned int i) { return i ? -EINVAL : 0; }

static const struct v4l2_ioctl_ops cf_ioctl_ops = {
	.vidioc_querycap = cf_querycap,
	.vidioc_enum_fmt_vid_cap = cf_enum_fmt,
	.vidioc_g_fmt_vid_cap = cf_g_fmt,
	.vidioc_s_fmt_vid_cap = cf_s_fmt,
	.vidioc_try_fmt_vid_cap = cf_try_fmt,
	.vidioc_enum_framesizes = cf_enum_framesizes,
	.vidioc_enum_frameintervals = cf_enum_frameintervals,
	.vidioc_g_parm = cf_g_parm,
	.vidioc_s_parm = cf_g_parm,
	.vidioc_enum_input = cf_enum_input,
	.vidioc_g_input = cf_g_input,
	.vidioc_s_input = cf_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations cf_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* --------------------------------------------------------------- feed device */
struct feed_file {
	struct camfeed *cf;
	int seen_gen;
};

static int feed_open(struct inode *inode, struct file *f)
{
	struct miscdevice *m = f->private_data; /* set by the misc core */
	struct feed_file *ff = kzalloc(sizeof(*ff), GFP_KERNEL);

	if (!ff)
		return -ENOMEM;
	ff->cf = container_of(m, struct camfeed, misc);
	ff->seen_gen = -1; /* first read reports the current state */
	f->private_data = ff;
	return 0;
}

static int feed_release(struct inode *inode, struct file *f)
{
	kfree(f->private_data);
	return 0;
}

static ssize_t feed_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct feed_file *ff = f->private_data;
	struct camfeed *cf = ff->cf;
	int gen;
	char c;

	if (!len)
		return 0;
	if (atomic_read(&cf->state_gen) == ff->seen_gen) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(cf->state_wq, atomic_read(&cf->state_gen) != ff->seen_gen))
			return -ERESTARTSYS;
	}
	gen = atomic_read(&cf->state_gen);
	c = READ_ONCE(cf->streaming) ? '1' : '0';
	ff->seen_gen = gen;
	if (copy_to_user(buf, &c, 1))
		return -EFAULT;
	return 1;
}

static void cf_complete(struct camfeed *cf, struct cf_buf *b, bool ok)
{
	b->vb.vb2_buf.timestamp = ktime_get_ns();
	b->vb.sequence = cf->sequence++;
	b->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&b->vb.vb2_buf, 0, cf->frame_size);
	vb2_buffer_done(&b->vb.vb2_buf, ok ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
}

static ssize_t feed_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct camfeed *cf = ((struct feed_file *)f->private_data)->cf;
	struct cf_buf *b = NULL;
	unsigned long flags;
	ssize_t ret = len;
	void *dst;

	if (len != cf->frame_size)
		return -EINVAL;
	mutex_lock(&cf->feed_lock);
	spin_lock_irqsave(&cf->qlock, flags);
	if (!list_empty(&cf->queued)) {
		b = list_first_entry(&cf->queued, struct cf_buf, list);
		list_del(&b->list);
	}
	spin_unlock_irqrestore(&cf->qlock, flags);
	if (!b)
		goto out; /* nobody waiting: drop the frame */
	dst = vb2_plane_vaddr(&b->vb.vb2_buf, 0);
	if (!dst || copy_from_user(dst, buf, len)) {
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		ret = -EFAULT;
		goto out;
	}
	cf_complete(cf, b, true);
out:
	mutex_unlock(&cf->feed_lock);
	return ret;
}

static __poll_t feed_poll(struct file *f, poll_table *wait)
{
	struct feed_file *ff = f->private_data;
	__poll_t m = EPOLLOUT | EPOLLWRNORM;

	poll_wait(f, &ff->cf->state_wq, wait);
	if (atomic_read(&ff->cf->state_gen) != ff->seen_gen)
		m |= EPOLLIN | EPOLLRDNORM;
	return m;
}

static long feed_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct camfeed *cf = ((struct feed_file *)f->private_data)->cf;
	void __user *up = (void __user *)arg;

	switch (cmd) {
	case CAMFEED_IOC_GETFMT: {
		struct camfeed_fmt fm = { .width = cf->cur_w, .height = cf->cur_h };

		return copy_to_user(up, &fm, sizeof(fm)) ? -EFAULT : 0;
	}
	case CAMFEED_IOC_EXPBUF: {
		struct camfeed_expbuf e;
		int ret;

		if (copy_from_user(&e, up, sizeof(e)))
			return -EFAULT;
		mutex_lock(&cf->lock);
		e.count = vb2_get_num_buffers(&cf->queue);
		e.size = cf->frame_size;
		ret = e.index < e.count ?
			vb2_core_expbuf(&cf->queue, &e.fd, cf->queue.type, vb2_get_buffer(&cf->queue, e.index),
					0, O_RDWR | O_CLOEXEC) : -EINVAL;
		mutex_unlock(&cf->lock);
		if (ret)
			return ret;
		return copy_to_user(up, &e, sizeof(e)) ? -EFAULT : 0;
	}
	case CAMFEED_IOC_ACQUIRE: {
		struct camfeed_buf a;
		struct cf_buf *b = NULL;
		unsigned long flags;
		long t;

		if (copy_from_user(&a, up, sizeof(a)))
			return -EFAULT;
		t = wait_event_interruptible_timeout(cf->buf_wq,
				!READ_ONCE(cf->streaming) || !list_empty(&cf->queued),
				msecs_to_jiffies(a.timeout_ms));
		if (t < 0)
			return t;
		mutex_lock(&cf->feed_lock);
		if (!cf->streaming) {
			mutex_unlock(&cf->feed_lock);
			return -ENODATA;
		}
		spin_lock_irqsave(&cf->qlock, flags);
		if (!list_empty(&cf->queued)) {
			b = list_first_entry(&cf->queued, struct cf_buf, list);
			list_del(&b->list);
		}
		spin_unlock_irqrestore(&cf->qlock, flags);
		if (!b) {
			mutex_unlock(&cf->feed_lock);
			return -EAGAIN;
		}
		a.index = b->vb.vb2_buf.index;
		cf->held[a.index] = b;
		/* write back and drop CPU lines before the GPU writes the pages behind them
		 * (the vmap helpers are no-ops on non-aliasing caches: clean+invalidate to PoC) */
		__cpuc_flush_dcache_area(vb2_plane_vaddr(&b->vb.vb2_buf, 0), vb2_plane_size(&b->vb.vb2_buf, 0));
		mutex_unlock(&cf->feed_lock);
		return copy_to_user(up, &a, sizeof(a)) ? -EFAULT : 0;
	}
	case CAMFEED_IOC_DONE: {
		struct camfeed_buf d;
		struct cf_buf *b;

		if (copy_from_user(&d, up, sizeof(d)))
			return -EFAULT;
		if (d.index >= VB2_MAX_FRAME)
			return -EINVAL;
		mutex_lock(&cf->feed_lock);
		b = cf->held[d.index];
		cf->held[d.index] = NULL;
		if (b) {
			/* the consumer must not see lines cached before the GPU wrote the frame */
			__cpuc_flush_dcache_area(vb2_plane_vaddr(&b->vb.vb2_buf, 0), vb2_plane_size(&b->vb.vb2_buf, 0));
			cf_complete(cf, b, d.ok);
		}
		mutex_unlock(&cf->feed_lock);
		return b ? 0 : -ENOENT; /* ENOENT: the stream stopped meanwhile */
	}
	}
	return -ENOTTY;
}

static const struct file_operations feed_fops = {
	.owner = THIS_MODULE,
	.open = feed_open,
	.release = feed_release,
	.read = feed_read,
	.write = feed_write,
	.poll = feed_poll,
	.unlocked_ioctl = feed_ioctl,
	.llseek = noop_llseek,
};

/* --------------------------------------------------------------- module */
static void cf_release(struct video_device *vdev) { }

static void cf_destroy(struct camfeed *cf)
{
	misc_deregister(&cf->misc);
	video_unregister_device(&cf->vdev);
	v4l2_device_unregister(&cf->v4l2);
	kfree(cf);
}

static int cf_create(unsigned int idx)
{
	struct camfeed *cf;
	int ret;

	if (!width[idx] || !height[idx] || (width[idx] & 1))
		return -EINVAL;
	cf = kzalloc(sizeof(*cf), GFP_KERNEL);
	if (!cf)
		return -ENOMEM;
	cf->card = dev_names[idx];
	cf->sizes[0] = (struct cf_size){ width[idx], height[idx] };
	cf->n_sizes = 1;
	for (unsigned int i = 0; i < ARRAY_SIZE(std_sizes); i++)
		if (std_sizes[i].w <= width[idx] && std_sizes[i].h <= height[idx] &&
		    (std_sizes[i].w != width[idx] || std_sizes[i].h != height[idx]))
			cf->sizes[cf->n_sizes++] = std_sizes[i];
	cf->cur_w = cf->sizes[0].w;
	cf->cur_h = cf->sizes[0].h;
	cf->frame_size = cf->cur_w * cf->cur_h * 2;
	mutex_init(&cf->lock);
	mutex_init(&cf->feed_lock);
	spin_lock_init(&cf->qlock);
	INIT_LIST_HEAD(&cf->queued);
	init_waitqueue_head(&cf->state_wq);
	init_waitqueue_head(&cf->buf_wq);

	snprintf(cf->v4l2.name, sizeof(cf->v4l2.name), "%s", cf->card);
	ret = v4l2_device_register(NULL, &cf->v4l2);
	if (ret)
		goto err_free;

	cf->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cf->queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	cf->queue.buf_struct_size = sizeof(struct cf_buf);
	cf->queue.ops = &cf_qops;
	cf->queue.mem_ops = &vb2_vmalloc_memops;
	cf->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	cf->queue.lock = &cf->lock;
	cf->queue.drv_priv = cf;
	ret = vb2_queue_init(&cf->queue);
	if (ret)
		goto err_v4l2;

	strscpy(cf->vdev.name, cf->card, sizeof(cf->vdev.name));
	cf->vdev.fops = &cf_fops;
	cf->vdev.ioctl_ops = &cf_ioctl_ops;
	cf->vdev.release = cf_release;
	cf->vdev.v4l2_dev = &cf->v4l2;
	cf->vdev.queue = &cf->queue;
	cf->vdev.lock = &cf->lock;
	cf->vdev.vfl_dir = VFL_DIR_RX;
	cf->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(&cf->vdev, cf);
	ret = video_register_device(&cf->vdev, VFL_TYPE_VIDEO, video_nr + idx);
	if (ret)
		goto err_v4l2;

	cf->misc.minor = MISC_DYNAMIC_MINOR;
	cf->misc.name = feed_names[idx];
	cf->misc.fops = &feed_fops;
	cf->misc.mode = 0660;
	ret = misc_register(&cf->misc);
	if (ret)
		goto err_vdev;

	cfs[idx] = cf;
	pr_info("superman-camfeed: %s %s up to %ux%u YUYV @%u fps (%u sizes), feed /dev/%s\n", cf->card,
		video_device_node_name(&cf->vdev), cf->cur_w, cf->cur_h, fps, cf->n_sizes, cf->misc.name);
	return 0;

err_vdev:
	video_unregister_device(&cf->vdev);
err_v4l2:
	v4l2_device_unregister(&cf->v4l2);
err_free:
	kfree(cf);
	return ret;
}

static int __init camfeed_init(void)
{
	int ret = 0;

	if (!devices || devices > MAX_DEV || !fps)
		return -EINVAL;
	for (unsigned int i = 0; i < devices && !ret; i++)
		ret = cf_create(i);
	if (ret)
		for (unsigned int i = 0; i < MAX_DEV; i++)
			if (cfs[i]) { cf_destroy(cfs[i]); cfs[i] = NULL; }
	return ret;
}

static void __exit camfeed_exit(void)
{
	for (unsigned int i = 0; i < MAX_DEV; i++)
		if (cfs[i])
			cf_destroy(cfs[i]);
}

module_init(camfeed_init);
module_exit(camfeed_exit);
MODULE_DESCRIPTION("Lumia 730 camera bridge: userspace-fed V4L2 capture devices");
MODULE_LICENSE("GPL");

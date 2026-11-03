// SPDX-License-Identifier: GPL-2.0
/* Laboratory software fence only: no MMIO, DMA, display or GPU operations. */
#include <linux/capability.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include "acquire-gate.h"

static bool allow_gate;
module_param(allow_gate, bool, 0400);
static atomic_t issued = ATOMIC_INIT(0);
struct gate {
	struct dma_fence fence;
	spinlock_t lock;
	struct timer_list timer;
};
static const char *gate_driver(struct dma_fence *f) { return "superman-lab-gate"; }
static const char *gate_timeline(struct dma_fence *f) { return "delayed-acquire"; }
static void gate_release(struct dma_fence *f) { kfree(container_of(f, struct gate, fence)); }
static const struct dma_fence_ops gate_ops = {
	.get_driver_name = gate_driver,
	.get_timeline_name = gate_timeline,
	.release = gate_release,
};
static void gate_signal(struct timer_list *timer)
{
	struct gate *g = container_of(timer, struct gate, timer);
	dma_fence_signal(&g->fence);
	pr_info("SUPERMAN_ACQUIRE_GATE signaled software_delay_ms=500\n");
	/* Timer owns the initial reference, even after client FD closure. */
	dma_fence_put(&g->fence);
}
static long gate_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct superman_acquire_gate request;
	struct sync_file *sync;
	struct gate *g;
	int fd, ret;
	if (!capable(CAP_SYS_ADMIN)) return -EPERM;
	if (cmd != SUPERMAN_ACQUIRE_GATE) return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request))) return -EFAULT;
	if (request.delay_ms != SUPERMAN_GATE_DELAY_MS || request.fence_fd != -1) return -EINVAL;
	if (atomic_cmpxchg(&issued, 0, 1)) return -EALREADY;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { ret = fd; goto reset; }
	g = kzalloc(sizeof(*g), GFP_KERNEL);
	if (!g) { ret = -ENOMEM; goto put_fd; }
	spin_lock_init(&g->lock);
	dma_fence_init(&g->fence, &gate_ops, &g->lock, dma_fence_context_alloc(1), 1);
	timer_setup(&g->timer, gate_signal, 0);
	sync = sync_file_create(&g->fence);
	if (!sync) { dma_fence_put(&g->fence); ret = -ENOMEM; goto put_fd; }
	request.fence_fd = fd;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		fput(sync->file); dma_fence_put(&g->fence);
		put_unused_fd(fd); atomic_set(&issued, 0); return -EFAULT;
	}
	/* The module is deliberately permanent; no timer or fence can outlive text. */
	mod_timer(&g->timer, jiffies + msecs_to_jiffies(SUPERMAN_GATE_DELAY_MS));
	fd_install(fd, sync->file);
	pr_info("SUPERMAN_ACQUIRE_GATE created software_delay_ms=500\n");
	return 0;
put_fd:
	put_unused_fd(fd);
reset:
	atomic_set(&issued, 0);
	return ret;
}
static const struct file_operations gate_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gate_ioctl,
	.compat_ioctl = gate_ioctl,
};
static struct miscdevice gate_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "superman-acquire-gate",
	.fops = &gate_fops, .mode = 0600,
};
static bool gpu_boot(void)
{
	struct device_node *node;
	const char *value;
	char *copy, *cursor, *token;
	int length, native = 0, gpu = 0, hwc = 0;
	bool ok = true;
	static const char exact[] = "nokia,superman\0qcom,msm8226";
	node = of_find_node_by_path("/");
	value = of_get_property(node, "compatible", &length);
	ok = value && length == sizeof(exact) && !memcmp(value, exact, sizeof(exact));
	of_node_put(node);
	if (!ok) return false;
	node = of_find_node_by_path("/chosen");
	value = of_get_property(node, "bootargs", &length);
	copy = value && length > 0 && value[length-1] == 0 ? kstrdup(value, GFP_KERNEL) : NULL;
	of_node_put(node);
	if (!copy) return false;
	cursor = copy;
	while ((token = strsep(&cursor, " \t\n"))) {
		if (!strncmp(token, "androidboot.superman_native=", sizeof("androidboot.superman_native=") - 1)) {
			if (strcmp(token, "androidboot.superman_native=p34")) ok = false;
			native++;
		} else if (!strncmp(token, "androidboot.superman_native_stage=", sizeof("androidboot.superman_native_stage=") - 1)) {
			if (!strcmp(token, "androidboot.superman_native_stage=gpu")) gpu++;
			else if (!strcmp(token, "androidboot.superman_native_stage=hwc")) hwc++;
			else ok = false;
		} else if (!strcmp(token, "androidboot.superman_native") || !strcmp(token, "androidboot.superman_native_stage")) ok = false;
	}
	kfree(copy);
	return ok && native && ((gpu && !hwc) || (hwc && !gpu));
}
static int __init gate_init(void)
{
	int ret;
	if (!allow_gate || !gpu_boot()) return -EPERM;
	ret = misc_register(&gate_misc);
	if (!ret) __module_get(THIS_MODULE); /* Intentionally retained until reboot. */
	return ret;
}
module_init(gate_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman sealed GPU/HWC-phase one-shot software acquire gate");

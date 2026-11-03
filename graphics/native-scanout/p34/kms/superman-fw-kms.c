// SPDX-License-Identifier: GPL-2.0
/* P34 laboratory warm-only KMS owner for Nokia 730 / MSM8226.
 * No DSI, PHY, PLL, panel, timing, reset or power initialization.
 * The normal MDSS driver remains disabled. The inherited simplefb supplier
 * remains bound and retains clocks/power. No fbdev emulation is registered.
 * At most three DMA-BUF backings are pinned at once. Prepared plane states
 * and the physically latched backing retain their pins. Old backing is freed
 * only after a verified subsequent latch and the matching plane cleanup.
 * A missing latch/vblank or fault quarantines every pin until reboot.
 * Not production-ready; compiled separately and excluded from product.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modes.h>
#include <drm/drm_prime.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include "scanout-contract.h"
#include "firmware-clock.h"

#define IRQ_EN 0x110
#define IRQ_STATUS 0x114
#define IRQ_CLEAR 0x118
#define VSYNC BIT(27)
#define UNDERFLOW BIT(26)
#define OWN_IRQ (VSYNC | UNDERFLOW)
#define CTL_FLUSH 0x618
#define RGB_ADDRESS 0x1e14
#define RGB_STRIDE 0x1e24
#define FRAME_COUNT 0x213ac
#define ORIGINAL_ADDRESS 0x03200000
#define ORIGINAL_STRIDE 2880
#define MAX_TARGETS 3

static bool allow_handoff;
module_param(allow_handoff, bool, 0444);
MODULE_PARM_DESC(allow_handoff, "Explicit laboratory boot acknowledgement; default false");

struct fw_gem {
	struct drm_gem_object base;
	struct sg_table *sgt;
	dma_addr_t address;
};
struct fw_target {
	struct drm_gem_object *object;
	unsigned int preparations;
};
struct fw_kms {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *regs;
	struct platform_device *supplier;
	struct device_link *supplier_link;
	struct clk *clocks[6];
	unsigned int clock_count;
	struct mutex pin_lock;
	struct fw_target targets[MAX_TARGETS];
	unsigned int target_count;
	bool module_pinned;
	bool ready, fault, pending, vblank_on;
	bool pending_restore, original_confirmed;
	struct dma_buf *active_backing, *pending_backing;
	u32 commit_frame, observed, original_frame;
	struct drm_pending_vblank_event *event;
	bool event_ref;
	wait_queue_head_t wait;
};
static struct fw_kms *to_fw(struct drm_device *drm)
{
	return container_of(drm, struct fw_kms, drm);
}
static struct fw_gem *to_gem(struct drm_gem_object *gem)
{
	return container_of(gem, struct fw_gem, base);
}
static u32 fw_read(struct fw_kms *fw, u32 reg)
{
	return readl(fw->regs + reg);
}
static void fw_write(struct fw_kms *fw, u32 reg, u32 value)
{
	writel(value, fw->regs + reg);
}
static const struct drm_display_mode recovered_mode = {
	.clock = 64549,
	.hdisplay = 720, .hsync_start = 780, .hsync_end = 784, .htotal = 824,
	.vdisplay = 1280, .vsync_start = 1295, .vsync_end = 1296, .vtotal = 1304,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	.name = "720x1280",
};
static int firmware_guard(struct fw_kms *fw)
{
	static const struct { u32 reg, value; const char *name; } expected[] = {
#include "firmware-state.inc"
	};
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		if (fw_read(fw, expected[i].reg) != expected[i].value) {
			dev_err(fw->drm.dev, "inherited %s mismatched\n", expected[i].name);
			return -EINVAL;
		}
	}
	/* Preserve the observed firmware pattern without clearing any bit.
	 * Normal MDP completion checks only the issued flush mask. P39 captured
	 * 0x1dfb7 with RGB0/CTL clear while frames advanced; all unexplained
	 * patterns and every IRQ mask still fail before any ownership write.
	 */
	{
		u32 irq_mask = fw_read(fw, IRQ_EN);
		u32 flush = fw_read(fw, CTL_FLUSH);
		if (!superman_inherited_owner_available(irq_mask, flush)) {
			dev_err(fw->drm.dev,
				"FW_OWNER irq_mask=%#x ctl0_flush=%#x owned_pending=%#x\n",
				irq_mask, flush, flush & SUPERMAN_OWN_FLUSH);
			return -EBUSY;
		}
		dev_info(fw->drm.dev,
			"FW_OWNER irq_mask=%#x ctl0_flush=%#x owned_pending=0\n",
			irq_mask, flush);
	}
	return 0;
}
static void fw_gem_free(struct drm_gem_object *object)
{
	struct fw_gem *gem = to_gem(object);
	drm_prime_gem_destroy(object, gem->sgt);
	drm_gem_object_release(object);
	kfree(gem);
}
static const struct drm_gem_object_funcs fw_gem_funcs = {
	.free = fw_gem_free,
};
static struct drm_gem_object *fw_import(struct drm_device *drm,
		struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct fw_gem *gem;
	struct scatterlist *sg;
	u64 next = sg_dma_address(sgt->sgl);
	unsigned int i;
	if (device_iommu_mapped(drm->dev) ||
	    drm_prime_get_contiguous_size(sgt) < attach->dmabuf->size)
		return ERR_PTR(-EINVAL);
	/* Validate physical identity, not merely a contiguous GPU/display IOVA. */
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		if (sg_dma_address(sg) != next || sg_dma_address(sg) != sg_phys(sg))
			return ERR_PTR(-EINVAL);
		next += sg_dma_len(sg);
	}
	gem = kzalloc(sizeof(*gem), GFP_KERNEL);
	if (!gem)
		return ERR_PTR(-ENOMEM);
	drm_gem_private_object_init(drm, &gem->base, attach->dmabuf->size);
	gem->base.funcs = &fw_gem_funcs;
	gem->sgt = sgt;
	gem->address = sg_dma_address(sgt->sgl);
	return &gem->base;
}
/* pin_lock excludes prepare/cleanup/drop. Never put a GEM under event_lock:
 * PRIME detach may sleep. A live prepared state prevents its backing from
 * disappearing while another commit is being prepared or queued.
 */
static void fw_prune_locked(struct fw_kms *fw)
{
	struct dma_buf *active;
	unsigned long flags;
	unsigned int i = 0;
	bool safe;
	spin_lock_irqsave(&fw->drm.event_lock, flags);
	safe = !fw->pending && !fw->fault &&
		(fw->original_confirmed || fw->active_backing);
	active = fw->active_backing;
	spin_unlock_irqrestore(&fw->drm.event_lock, flags);
	if (!safe)
		return;
	while (i < fw->target_count) {
		struct fw_target *target = &fw->targets[i];
		struct drm_gem_object *object = target->object;
		if (target->preparations || object->import_attach->dmabuf == active) {
			i++;
			continue;
		}
		fw->targets[i] = fw->targets[--fw->target_count];
		memset(&fw->targets[fw->target_count], 0, sizeof(*target));
		drm_gem_object_put(object);
		pr_info("FW_PIN released_backing count=%u owner_state_confirmed=1 preparations=0\n",
			fw->target_count);
	}
}
static int fw_pin(struct fw_kms *fw, struct drm_gem_object *object)
{
	unsigned int i;
	int ret = 0;
	if (!object || object->funcs != &fw_gem_funcs ||
	    !object->import_attach || !object->import_attach->dmabuf)
		return -EINVAL;
	mutex_lock(&fw->pin_lock);
	fw_prune_locked(fw);
	for (i = 0; i < fw->target_count; i++) {
		/* GEM_CLOSE removes the file's PRIME cache entry. A later import
		 * can create another GEM for the same backing. Keep the original
		 * GEM/attachment reference while prepared or physically active.
		 */
		if (fw->targets[i].object->import_attach->dmabuf ==
		    object->import_attach->dmabuf) {
			if (to_gem(fw->targets[i].object)->address != to_gem(object)->address ||
			    fw->targets[i].object->size != object->size) {
				ret = -EINVAL;
				pr_err("FW_PIN backing_extent_changed slot=%u\n", i);
			} else {
				if (fw->targets[i].preparations == UINT_MAX) {
					ret = -EOVERFLOW;
					goto out;
				}
				fw->targets[i].preparations++;
				pr_info("FW_PIN reused_backing slot=%u count=%u\n",
					i, fw->target_count);
			}
			goto out;
		}
	}
	if (fw->target_count == MAX_TARGETS) {
		ret = -ENOSPC;
		pr_err("FW_PIN distinct_backing_limit count=%u\n", fw->target_count);
		goto out;
	}
	if (!fw->module_pinned) {
		if (!try_module_get(THIS_MODULE)) {
			ret = -ENODEV;
			goto out;
		}
		fw->module_pinned = true;
	}
	drm_gem_object_get(object);
	fw->targets[fw->target_count++] = (struct fw_target){object, 1};
	pr_info("FW_PIN retained_backing count=%u\n", fw->target_count);
	out:
	mutex_unlock(&fw->pin_lock);
	return ret;
}
static int fw_drop_targets(struct fw_kms *fw)
{
	if (READ_ONCE(fw->pending) || READ_ONCE(fw->fault) ||
	    !READ_ONCE(fw->original_confirmed))
		return -EBUSY;
	mutex_lock(&fw->pin_lock);
	fw_prune_locked(fw);
	/* The old plane is still prepared during disable. Its later cleanup
	 * releases the last pin; concurrent prepared states remain protected.
	 */
	/* Keep the one module reference: IRQ/supplier ownership lasts until reboot. */
	mutex_unlock(&fw->pin_lock);
	return 0;
}
static void fw_cleanup_fb(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *plane)
{
	struct fw_kms *fw = to_fw(pipe->crtc.dev);
	struct drm_gem_object *object;
	unsigned int i;
	if (!plane->fb)
		return;
	object = drm_gem_fb_get_obj(plane->fb, 0);
	mutex_lock(&fw->pin_lock);
	for (i = 0; i < fw->target_count; i++) {
		if (fw->targets[i].object->import_attach->dmabuf !=
		    object->import_attach->dmabuf)
			continue;
		if (WARN_ON(!fw->targets[i].preparations)) {
			WRITE_ONCE(fw->fault, true);
			break;
		}
		fw->targets[i].preparations--;
		fw_prune_locked(fw);
		mutex_unlock(&fw->pin_lock);
		return;
	}
	/* A successful prepare must always have its matching retained entry. */
	WRITE_ONCE(fw->fault, true);
	pr_err("FW_PIN cleanup_unmatched retain_until_reboot=1\n");
	mutex_unlock(&fw->pin_lock);
}
static bool fw_fb_valid(struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	return obj && obj->funcs == &fw_gem_funcs && fb->width == 720 &&
		fb->height == 1280 && fb->format->format == DRM_FORMAT_ARGB8888 &&
		fb->modifier == DRM_FORMAT_MOD_LINEAR && !fb->offsets[0] &&
		superman_target_valid(to_gem(obj)->address, obj->size, fb->pitches[0]);
}
static const struct drm_framebuffer_funcs fw_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};
static struct drm_framebuffer *fw_fb_create(struct drm_device *drm,
		struct drm_file *file, const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_framebuffer *fb;
	int ret;
	if (cmd->width != 720 || cmd->height != 1280 ||
	    cmd->pixel_format != DRM_FORMAT_ARGB8888 || cmd->offsets[0] ||
	    cmd->modifier[0] != DRM_FORMAT_MOD_LINEAR)
		return ERR_PTR(-EINVAL);
	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_fb_init_with_funcs(drm, fb, file, cmd, &fw_fb_funcs);
	if (ret) {
		kfree(fb);
		return ERR_PTR(ret);
	}
	if (!fw_fb_valid(fb)) {
		drm_framebuffer_put(fb);
		return ERR_PTR(-EINVAL);
	}
	return fb;
}
static enum drm_mode_status fw_mode_valid(struct drm_simple_display_pipe *pipe,
		const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &recovered_mode) ? MODE_OK : MODE_BAD;
}
static int fw_check(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *plane, struct drm_crtc_state *crtc)
{
	if (!drm_mode_equal(&crtc->mode, &recovered_mode))
		return -EINVAL;
	return plane->fb && fw_fb_valid(plane->fb) ? 0 : -EINVAL;
}
static int fw_prepare_fb(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *plane)
{
	int ret;
	if (!plane->fb)
		return 0;
	ret = drm_gem_plane_helper_prepare_fb(&pipe->plane, plane);
	if (ret)
		return ret;
	return fw_pin(to_fw(pipe->crtc.dev), drm_gem_fb_get_obj(plane->fb, 0));
}
/* Event lock serializes IRQ retirement and the tiny ownership state machine.
 * Snapshot FRAME_COUNT after the posted flush. Retire only on a subsequent
 * physical frame and cleared flush. No immediate/fabricated event is sent.
 */
static void fw_queue(struct fw_kms *fw, struct drm_framebuffer *fb)
{
	struct drm_crtc *crtc = &fw->pipe.crtc;
	unsigned long flags;
	bool event_ref = crtc->state->event && !drm_crtc_vblank_get(crtc);
	u32 address = ORIGINAL_ADDRESS, stride = ORIGINAL_STRIDE;
	if (fb) {
		address = to_gem(drm_gem_fb_get_obj(fb, 0))->address;
		stride = fb->pitches[0];
	}
	spin_lock_irqsave(&fw->drm.event_lock, flags);
	if (WARN_ON(fw->pending)) {
		fw->fault = true;
		spin_unlock_irqrestore(&fw->drm.event_lock, flags);
		if (event_ref) drm_crtc_vblank_put(crtc);
		return;
	}
	fw->event = crtc->state->event;
	crtc->state->event = NULL;
	fw->event_ref = event_ref;
	if (fw->event && !fw->event_ref)
		fw->fault = true;
	fw->pending = true;
	fw->pending_restore = !fb;
	fw->pending_backing = fb ? drm_gem_fb_get_obj(fb, 0)->import_attach->dmabuf : NULL;
	if (!fw->fault) {
		fw->original_confirmed = false;
		fw_write(fw, RGB_STRIDE, stride);
		fw_write(fw, RGB_ADDRESS, address);
		/* Hardware CTL0 at absolute fd900600; flush RGB0 plus CTL itself. */
		fw_write(fw, CTL_FLUSH, SUPERMAN_OWN_FLUSH);
		fw_read(fw, CTL_FLUSH);
		fw->commit_frame = fw_read(fw, FRAME_COUNT);
	}
	spin_unlock_irqrestore(&fw->drm.event_lock, flags);
}
static void fw_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *crtc, struct drm_plane_state *plane)
{
	struct fw_kms *fw = to_fw(pipe->crtc.dev);
	drm_crtc_vblank_on(&pipe->crtc);
	WRITE_ONCE(fw->vblank_on, true);
	fw_queue(fw, plane->fb);
}
static void fw_update(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *old_plane)
{
	if (!pipe->crtc.state->active || pipe->crtc.state->active_changed)
		return;
	fw_queue(to_fw(pipe->crtc.dev), pipe->plane.state->fb);
}
static void fw_disable(struct drm_simple_display_pipe *pipe)
{
	struct fw_kms *fw = to_fw(pipe->crtc.dev);
	fw_queue(fw, NULL);
	if (!wait_event_timeout(fw->wait, !READ_ONCE(fw->pending), msecs_to_jiffies(500))) {
		WRITE_ONCE(fw->fault, true);
		dev_err(fw->drm.dev, "restore unconfirmed; retain all backing until reboot\n");
		return;
	}
	if (fw_drop_targets(fw)) {
		WRITE_ONCE(fw->fault, true);
		dev_err(fw->drm.dev, "restore state unconfirmed; retain all backing until reboot\n");
		return;
	}
	/* The original firmware buffer is restored, not powered off. The physical
	 * CRTC and ownership IRQ remain running, as at the guarded initial probe.
	 * drm_crtc_vblank_off() would block later waits until another atomic enable,
	 * preventing SF from receiving real vsync before its first client target.
	 * Leave accounting available; ordinary DRM idle accounting can still pause
	 * it through fw_disable_vblank without blocking a subsequent vblank_get.
	 */
	dev_info(fw->drm.dev, "FW_VBLANK accounting=available original_restore=confirmed\n");
}
static int fw_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct fw_kms *fw = to_fw(pipe->crtc.dev);
	if (READ_ONCE(fw->fault)) return -EIO;
	WRITE_ONCE(fw->vblank_on, true);
	return 0;
}
static void fw_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	/* Keep ownership IRQs running; pause only DRM accounting. */
	WRITE_ONCE(to_fw(pipe->crtc.dev)->vblank_on, false);
}
static const struct drm_simple_display_pipe_funcs fw_pipe_funcs = {
	.mode_valid = fw_mode_valid,
	.check = fw_check,
	.prepare_fb = fw_prepare_fb,
	.cleanup_fb = fw_cleanup_fb,
	.enable = fw_enable,
	.disable = fw_disable,
	.update = fw_update,
	.enable_vblank = fw_enable_vblank,
	.disable_vblank = fw_disable_vblank,
};
static irqreturn_t fw_irq(int irq, void *data)
{
	struct fw_kms *fw = data;
	u32 status = fw_read(fw, IRQ_STATUS) & OWN_IRQ;
	unsigned long flags;
	bool put = false;
	if (!status)
		return IRQ_NONE;
	fw_write(fw, IRQ_CLEAR, status);
	spin_lock_irqsave(&fw->drm.event_lock, flags);
	if (status & UNDERFLOW)
		fw->fault = true;
	if (status & VSYNC)
		fw->observed++;
	spin_unlock_irqrestore(&fw->drm.event_lock, flags);
	if ((status & VSYNC) && READ_ONCE(fw->vblank_on))
		drm_crtc_handle_vblank(&fw->pipe.crtc);
	spin_lock_irqsave(&fw->drm.event_lock, flags);
	if ((status & VSYNC) && superman_flip_retired(fw->pending, fw->fault,
		fw->commit_frame, fw_read(fw, FRAME_COUNT), fw_read(fw, CTL_FLUSH))) {
		put = fw->event_ref;
		fw->event_ref = false;
		fw->original_confirmed = fw->pending_restore;
		fw->active_backing = fw->pending_backing;
		fw->pending_backing = NULL;
		fw->pending = false;
		/* Completion wakes the commit tail on another CPU immediately. */
		if (fw->event)
			drm_crtc_send_vblank_event(&fw->pipe.crtc, fw->event);
		fw->event = NULL;
		wake_up(&fw->wait);
	}
	spin_unlock_irqrestore(&fw->drm.event_lock, flags);
	if (put)
		drm_crtc_vblank_put(&fw->pipe.crtc);
	wake_up(&fw->wait);
	return IRQ_HANDLED;
}
static int fw_atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct fw_kms *fw = to_fw(drm);
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&drm->event_lock, flags);
	if (!fw->ready || fw->fault)
		ret = -EIO;
	else if (fw->pending)
		ret = -EBUSY;
	spin_unlock_irqrestore(&drm->event_lock, flags);
	return ret ?: drm_atomic_helper_check(drm, state);
}
static void fw_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;
	struct fw_kms *fw = to_fw(drm);
	unsigned long flags;
	bool timed_out;
	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	/* Never fake vblank. Timeout retains physical backing pins until reboot. */
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_flip_done(drm, state);
	/* Serialize timeout classification with IRQ retirement. */
	spin_lock_irqsave(&drm->event_lock, flags);
	timed_out = fw->pending;
	if (timed_out)
		fw->fault = true;
	spin_unlock_irqrestore(&drm->event_lock, flags);
	if (timed_out)
		dev_err(drm->dev, "FW_PIN latch_timeout retain_until_reboot=1\n");
	drm_atomic_helper_cleanup_planes(drm, state);
}
static const struct drm_mode_config_helper_funcs fw_mode_helpers = {
	.atomic_commit_tail = fw_commit_tail,
};
static int fw_atomic_commit(struct drm_device *drm, struct drm_atomic_state *state,
			    bool nonblock)
{
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	int i, ret;
	long waited;

	/* Initial laboratory owner admits one serialized, blocking presenter. */
	if (nonblock || state->async_update)
		return -EOPNOTSUPP;
	/* A signaled fence can carry a failed GPU job. Check explicit acquire
	 * status before the helper consumes its fence and swaps display state.
	 * Keep the fence attached for the helper's ordinary lifetime handling.
	 */
	for_each_new_plane_in_state(state, plane, plane_state, i) {
		if (!plane_state->fence)
			continue;
		waited = dma_fence_wait_timeout(plane_state->fence, true,
					      msecs_to_jiffies(4000));
		if (waited <= 0) {
			ret = waited < 0 ? waited : -ETIMEDOUT;
			dev_err(drm->dev, "FW_ACQUIRE rejected wait=%d state_unswapped=1\n", ret);
			return ret;
		}
		ret = dma_fence_get_status(plane_state->fence);
		if (ret <= 0) {
			dev_err(drm->dev, "FW_ACQUIRE rejected status=%d state_unswapped=1\n", ret);
			return ret ?: -EIO;
		}
	}
	return drm_atomic_helper_commit(drm, state, false);
}
static const struct drm_mode_config_funcs fw_mode_funcs = {
	.fb_create = fw_fb_create,
	.atomic_check = fw_atomic_check,
	.atomic_commit = fw_atomic_commit,
};
static int fw_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &recovered_mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	return 1;
}
static enum drm_connector_status fw_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}
static const struct drm_connector_helper_funcs fw_connector_helpers = {
	.get_modes = fw_get_modes,
};
static const struct drm_connector_funcs fw_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = fw_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};
DEFINE_DRM_GEM_FOPS(fw_fops);
static const struct drm_driver fw_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &fw_fops,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_import_sg_table = fw_import,
	.name = "superman-fw-kms", .desc = "Nokia730 inherited firmware scanout",
	.major = 0, .minor = 1,
};
static void fw_release_clocks(struct fw_kms *fw)
{
	while (fw->clock_count) {
		struct clk *clk = fw->clocks[--fw->clock_count];
		clk_put(clk);
	}
}
static int fw_probe_error(struct device *dev, const char *stage, int ret)
{
	dev_err(dev, "FW_PROBE stage=%s ret=%d\n", stage, ret);
	return ret;
}
static int fw_clock_guard(struct fw_kms *fw)
{
	void __iomem *mmcc;
	struct fw_clock_rates rates;
	u32 first[FW_CLOCK_COUNT], second[FW_CLOCK_COUNT];
	unsigned long byte, pixel;
	unsigned int i, bad;
	/* Shared, read-only observation of MMCC. Its clock driver retains resource
	 * ownership; this mapping never requests the region or programs it.
	 * The board/boot/simplefb gates have already passed. No DSI provider is
	 * registered and no clock prepare/enable/rate/parent operation is issued.
	 */
	mmcc = devm_ioremap(fw->drm.dev, 0xfd8c2000, 0x1000);
	if (!mmcc)
		return -ENOMEM;
	for (i = 0; i < FW_CLOCK_COUNT; i++) {
		const struct fw_clock_register *r = &fw_clock_registers[i];
		first[i] = readl((r->mmcc ? mmcc : fw->regs) + r->offset);
	}
	for (i = 0; i < FW_CLOCK_COUNT; i++) {
		const struct fw_clock_register *r = &fw_clock_registers[i];
		second[i] = readl((r->mmcc ? mmcc : fw->regs) + r->offset);
		if (first[i] != second[i]) {
			dev_err(fw->drm.dev, "FW_CLOCK unstable %s\n", r->name);
			return -EAGAIN;
		}
	}
	bad = fw_clock_decode(first, &rates);
	if (bad != FW_CLOCK_COUNT) {
		dev_err(fw->drm.dev, "FW_CLOCK %s=%#x expected=%#x\n",
			fw_clock_registers[bad].name, first[bad],
			fw_clock_registers[bad].expected);
		return -EINVAL;
	}
	if (!fw_clock_rate_matches(rates.byte, 48411987) ||
	    !fw_clock_rate_matches(rates.pixel, recovered_mode.clock * 1000ULL))
		return -EINVAL;
	/* clk_get_rate is cached only when NOCACHE is absent. Do not cause a
	 * provider recalculation/runtime resume to recover missing parent data.
	 * Existing simplefb DT indexes BYTE0=2 and PCLK0=4 are preserved.
	 */
	if ((clk_hw_get_flags(__clk_get_hw(fw->clocks[2])) |
	     clk_hw_get_flags(__clk_get_hw(fw->clocks[4]))) & CLK_GET_RATE_NOCACHE)
		return -EOPNOTSUPP;
	byte = clk_get_rate(fw->clocks[2]);
	pixel = clk_get_rate(fw->clocks[4]);
	dev_info(fw->drm.dev,
		"FW_CLOCK physical byte=%llu pixel=%llu vco=%llu cached byte=%lu pixel=%lu\n",
		rates.byte, rates.pixel, rates.vco, byte, pixel);
	if (!fw_clock_cache_matches(byte, rates.byte) ||
	    !fw_clock_cache_matches(pixel, rates.pixel))
		return -EINVAL;
	return 0;
}
static int fw_supplier_get(struct fw_kms *fw, struct device_node *node)
{
	struct device_node *fb, *mdss;
	struct resource res;
	unsigned int i = 0;
	const char *stage = "normal-mdss";
	int ret = -EINVAL;
	u32 width, height, stride;
	const char *format;
	mdss = of_parse_phandle(node, "superman,normal-mdss", 0);
	if (!mdss || of_device_is_available(mdss)) {
		of_node_put(mdss);
		return fw_probe_error(fw->drm.dev, "supplier-normal-mdss", -EBUSY);
	}
	of_node_put(mdss);
	stage = "simplefb-geometry";
	fb = of_parse_phandle(node, "superman,retained-framebuffer", 0);
	if (!fb || !of_device_is_compatible(fb, "simple-framebuffer"))
		goto out;
	if (of_address_to_resource(fb, 0, &res) || res.start != ORIGINAL_ADDRESS ||
	    resource_size(&res) != 0x800000 ||
	    of_property_read_u32(fb, "width", &width) || width != 720 ||
	    of_property_read_u32(fb, "height", &height) || height != 1280 ||
	    of_property_read_u32(fb, "stride", &stride) || stride != ORIGINAL_STRIDE ||
	    of_property_read_string(fb, "format", &format) || strcmp(format, "a8r8g8b8"))
		goto out;
	stage = "simplefb-bound";
	fw->supplier = of_find_device_by_node(fb);
	if (!fw->supplier || !fw->supplier->dev.driver ||
	    strcmp(fw->supplier->dev.driver->name, "simple-framebuffer")) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	stage = "clock-count";
	if (of_clk_get_parent_count(fb) != 6)
		goto out;
	for (i = 0; i < 6; i++) {
		struct clk *clk;
		stage = "clock-reference";
		clk = of_clk_get(fb, i);
		if (IS_ERR(clk)) { ret = PTR_ERR(clk); goto out; }
		/* Borrow only references to clocks owned by retained simplefb.
		 * No prepare/enable is issued: hardware-enable bits alone do
		 * not prove a new prepare would preserve the firmware PLL.
		 */
		stage = "clock-ready";
		if (!clk_hw_is_prepared(__clk_get_hw(clk)) || !__clk_is_enabled(clk)) {
			clk_put(clk); ret = -EINVAL; goto out;
		}
		fw->clocks[fw->clock_count++] = clk;
	}
	stage = "supplier-link";
	fw->supplier_link = device_link_add(fw->drm.dev, &fw->supplier->dev,
		DL_FLAG_AUTOREMOVE_CONSUMER);
	ret = fw->supplier_link ? 0 : -ENOMEM;
	out:
	if (ret)
		dev_err(fw->drm.dev, "FW_SUPPLIER stage=%s index=%u ret=%d\n", stage, i, ret);
	of_node_put(fb);
	return ret;
}
static bool fw_boot_allowed(void)
{
	struct device_node *root, *chosen;
	const void *compatible;
	const char *args, *token;
	struct task_struct *task;
	char comm[TASK_COMM_LEN];
	int length;
	bool allowed = false;
	static const char expected[] = "nokia,superman\0qcom,msm8226";
	root = of_find_node_by_path("/");
	compatible = of_get_property(root, "compatible", &length);
	if (!compatible || length != sizeof(expected) ||
	    memcmp(compatible, expected, sizeof(expected)))
		goto root_out;
	chosen = of_find_node_by_path("/chosen");
	if (of_property_read_string(chosen, "bootargs", &args))
		goto chosen_out;
	token = strstr(args, "androidboot.superman_native=p34");
	if (!token || (token != args && token[-1] != ' ') ||
	    (token[31] && token[31] != ' '))
		goto chosen_out;
	allowed = true;
	rcu_read_lock();
	for_each_process(task) {
		get_task_comm(comm, task);
		if (!strcmp(comm, "surfaceflinger")) {
			allowed = false;
			break;
		}
	}
	rcu_read_unlock();
chosen_out:
	of_node_put(chosen);
root_out:
	of_node_put(root);
	return allowed;
}
static int fw_probe(struct platform_device *pdev)
{
	struct fw_kms *fw;
	struct resource *resource;
	int ret, irq;
	bool inherited_vblank_on = false;
	const char *stage = "supplier";
	static const u32 formats[] = { DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	if (!allow_handoff || !of_property_read_bool(pdev->dev.of_node,
		"superman,firmware-initialized") ||
	    !fw_boot_allowed())
		return fw_probe_error(&pdev->dev, "boot-ack", -EPERM);
	if (device_iommu_mapped(&pdev->dev) ||
	    of_find_property(pdev->dev.of_node, "iommus", NULL))
		return fw_probe_error(&pdev->dev, "display-iommu", -EINVAL);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return fw_probe_error(&pdev->dev, "dma-mask", ret);
	fw = devm_drm_dev_alloc(&pdev->dev, &fw_driver, struct fw_kms, drm);
	if (IS_ERR(fw))
		return fw_probe_error(&pdev->dev, "drm-allocation", PTR_ERR(fw));
	mutex_init(&fw->pin_lock);
	init_waitqueue_head(&fw->wait);
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != 0xfd900000 || resource_size(resource) != 0x23000)
		return fw_probe_error(&pdev->dev, "register-resource", -EINVAL);
	fw->regs = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(fw->regs))
		return fw_probe_error(&pdev->dev, "register-map", PTR_ERR(fw->regs));
	ret = fw_supplier_get(fw, pdev->dev.of_node);
	if (ret)
		goto clocks;
	stage = "inherited-state";
	ret = firmware_guard(fw);
	if (ret)
		goto clocks;
	stage = "physical-clocks";
	ret = fw_clock_guard(fw);
	if (ret)
		goto clocks;
	stage = "mode-config";
	ret = drmm_mode_config_init(&fw->drm);
	if (ret)
		goto clocks;
	fw->drm.mode_config.funcs = &fw_mode_funcs;
	fw->drm.mode_config.helper_private = &fw_mode_helpers;
	fw->drm.mode_config.min_width = fw->drm.mode_config.max_width = 720;
	fw->drm.mode_config.min_height = fw->drm.mode_config.max_height = 1280;
	stage = "connector";
	ret = drm_connector_init(&fw->drm, &fw->connector, &fw_connector_funcs,
		DRM_MODE_CONNECTOR_DSI);
	if (ret)
		goto clocks;
	drm_connector_helper_add(&fw->connector, &fw_connector_helpers);
	stage = "display-pipe";
	ret = drm_simple_display_pipe_init(&fw->drm, &fw->pipe, &fw_pipe_funcs,
		formats, ARRAY_SIZE(formats), modifiers, &fw->connector);
	if (ret)
		goto clocks;
	stage = "vblank-init";
	ret = drm_vblank_init(&fw->drm, 1);
	if (ret)
		goto clocks;
	drm_mode_config_reset(&fw->drm);
	stage = "irq-resource";
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) { ret = irq; goto clocks; }
	stage = "irq-request";
	ret = devm_request_irq(&pdev->dev, irq, fw_irq, 0, "superman-fw-kms", fw);
	if (ret)
		goto clocks;
	stage = "real-vblank";
	fw->original_frame = fw_read(fw, FRAME_COUNT);
	fw_write(fw, IRQ_CLEAR, fw_read(fw, IRQ_STATUS) & OWN_IRQ);
	fw_write(fw, IRQ_EN, OWN_IRQ);
	/* Actual interrupt plus actual advancing physical frame is mandatory. */
	if (!wait_event_timeout(fw->wait, READ_ONCE(fw->observed) >= 4 ||
		READ_ONCE(fw->fault), msecs_to_jiffies(250)) || fw->fault ||
	    (s32)(fw_read(fw, FRAME_COUNT) - fw->original_frame) < 3) {
		ret = -ETIMEDOUT;
		goto irq_off;
	}
	fw->original_confirmed = true;
	fw->ready = true;
	/* mode_config_reset blocked DRM accounting. The inherited physical CRTC
	 * is already running and has now passed the exclusive real IRQ/frame gate.
	 * Reflect that state without enabling an atomic mode or writing a target.
	 */
	drm_crtc_vblank_on(&fw->pipe.crtc);
	inherited_vblank_on = true;
	dev_info(&pdev->dev, "FW_VBLANK accounting=available inherited_irq_gate=passed\n");
	stage = "drm-register";
	ret = drm_dev_register(&fw->drm, 0);
	if (ret)
		goto irq_off;
	platform_set_drvdata(pdev, fw);
	dev_info(&pdev->dev, "guarded warm KMS ready; no target address written\n");
	return 0;
	irq_off:
	if (inherited_vblank_on)
		drm_crtc_vblank_off(&fw->pipe.crtc);
	fw_write(fw, IRQ_EN, 0);
	fw_write(fw, IRQ_CLEAR, fw_read(fw, IRQ_STATUS) & OWN_IRQ);
	synchronize_irq(irq);
	clocks:
	fw_probe_error(&pdev->dev, stage, ret);
	fw_release_clocks(fw);
	if (fw->supplier)
		put_device(&fw->supplier->dev);
	return ret;
}
static void fw_remove(struct platform_device *pdev)
{
	struct fw_kms *fw = platform_get_drvdata(pdev);
	/* Module self-reference and suppressed bind attributes prevent ordinary
	 * unload/unbind after imports. Forced removal is unsupported laboratory use.
	 */
	WARN_ON(fw->module_pinned);
	drm_dev_unregister(&fw->drm);
	drm_crtc_vblank_off(&fw->pipe.crtc);
	fw_write(fw, IRQ_EN, 0);
	synchronize_irq(platform_get_irq(pdev, 0));
	fw_release_clocks(fw);
	put_device(&fw->supplier->dev);
}
static int fw_suspend(struct device *dev)
{
	/* No panel suspend/cold-init sequence exists. Reject rather than reset. */
	return -EBUSY;
}
static int fw_resume(struct device *dev)
{
	/* Only the abort of a rejected suspend is supported, with no hardware writes. */
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(fw_pm_ops, fw_suspend, fw_resume);
static const struct of_device_id fw_of_match[] = {
	{ .compatible = "nokia,superman-firmware-kms" }, { }
};
MODULE_DEVICE_TABLE(of, fw_of_match);
static struct platform_driver fw_platform_driver = {
	.probe = fw_probe, .remove = fw_remove,
	.driver = { .name = "superman-fw-kms", .of_match_table = fw_of_match,
		.pm = pm_sleep_ptr(&fw_pm_ops), .suppress_bind_attrs = true },
};
module_platform_driver(fw_platform_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("P34 Nokia730 firmware-only KMS laboratory candidate");

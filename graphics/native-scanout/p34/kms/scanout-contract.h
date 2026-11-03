/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SUPERMAN_SCANOUT_CONTRACT_H
#define SUPERMAN_SCANOUT_CONTRACT_H
/* These pure predicates are exercised with boundary and failure cases. */
#define SUPERMAN_OWN_FLUSH 0x20008U
#define SUPERMAN_INHERITED_OTHER_FLUSH 0x1dfb7U
/* P40: exactly the P39 captured state (or fully clear), never arbitrary
 * unrelated bits. No IRQ mask or owned RGB0/CTL pending bit is permitted.
 * Accepting this metadata does not establish IRQ ownership or real vblank.
 */
static inline bool superman_inherited_owner_available(u32 irq_mask, u32 flush)
{
	return !irq_mask && !(flush & SUPERMAN_OWN_FLUSH) &&
		(!flush || flush == SUPERMAN_INHERITED_OTHER_FLUSH);
}

static inline bool superman_target_valid(u64 address, u64 size, u32 stride)
{
	u64 extent;
	if (!address || address > 0xffffffffULL || stride < 2880 ||
	    stride > 65535 || (stride & 3))
		return false;
	extent = (u64)stride * 1279 + 2880;
	if (extent > size || extent > 0x100000000ULL - address)
		return false;
	/* Never use the protected inherited 8 MiB framebuffer as a target. */
	return address + extent <= 0x03200000ULL || address >= 0x03a00000ULL;
}
static inline bool superman_flip_retired(bool pending, bool fault,
				       u32 first_frame, u32 now, u32 flush)
{
	/* Modular comparison permits wrap; require a later physical frame. */
	return pending && !fault && (s32)(now - first_frame) > 0 && !(flush & SUPERMAN_OWN_FLUSH);
}
#endif

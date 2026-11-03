/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef SUPERMAN_ACQUIRE_GATE_H
#define SUPERMAN_ACQUIRE_GATE_H
#include <linux/types.h>
#include <linux/ioctl.h>
/* ARM32/compat invariant: eight bytes, no pointers. One gate per boot. */
struct superman_acquire_gate {
	__u32 delay_ms;
	__s32 fence_fd;
};
#define SUPERMAN_ACQUIRE_GATE _IOWR('S', 0x43, struct superman_acquire_gate)
#define SUPERMAN_GATE_DELAY_MS 500
#endif

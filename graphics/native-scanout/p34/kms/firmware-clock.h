/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SUPERMAN_FIRMWARE_CLOCK_H
#define SUPERMAN_FIRMWARE_CLOCK_H

/* P39: pure decoder for the exact P32 Nokia730 inherited clock capture.
 * These offsets are an immutable read allowlist, not initialization values.
 * PLL offsets are within fd900000+23000; MMCC offsets use fd8c2000+1000.
 * A matching snapshot is metadata evidence; real advancing IRQ/frame counters
 * remain required by the caller. No cold-init or PLL-lock proof is claimed.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/math64.h>
#define fw_clock_div(n, d) div_u64((n), (d))
#else
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define fw_clock_div(n, d) ((n) / (d))
#endif

#define FW_CLOCK_COUNT 14
static const struct fw_clock_register {
	u32 offset, expected;
	bool mmcc;
	const char *name;
} fw_clock_registers[FW_CLOCK_COUNT] = {
	{ 0x22a00, 1, false, "PLL_REFCLK" },
	{ 0x22a04, 0, false, "PLL_POSTDIV1" },
	{ 0x22a10, 2, false, "PLL_VREG" },
	{ 0x22a20, 0x1f, false, "PLL_GLB" },
	{ 0x22a28, 5, false, "PLL_POSTDIV3" },
	{ 0x22a38, 0, false, "PLL_SDM0" },
	{ 0x22a3c, 9, false, "PLL_SDM1" },
	{ 0x22a40, 0xf9, false, "PLL_SDM2" },
	{ 0x22a44, 0x15, false, "PLL_SDM3" },
	{ 0x004, 0x100, true, "PCLK0_CFG" },
	{ 0x008, 0, true, "PCLK0_M" },
	{ 0x00c, 0, true, "PCLK0_N" },
	{ 0x010, 0, true, "PCLK0_D" },
	{ 0x124, 0x100, true, "BYTE0_CFG" },
};

struct fw_clock_rates { u64 reference, vco, pixel, byte; };

/* Returns the first mismatched register, or FW_CLOCK_COUNT on success. */
static unsigned int fw_clock_decode(const u32 *v, struct fw_clock_rates *r)
{
	u64 seed, byte_parent;
	unsigned int i;
	for (i = 0; i < FW_CLOCK_COUNT; i++)
		if (v[i] != fw_clock_registers[i].expected)
			return i;
	r->reference = 19200000ULL * (1 + (v[0] & 1));
	seed = ((v[8] & 255) << 8) | (v[7] & 255);
	r->vco = r->reference * ((v[6] & 63) + 1) +
		((r->reference * seed) >> 16);
	r->pixel = fw_clock_div(r->vco, (v[4] & 255) + 1);
	byte_parent = fw_clock_div(r->vco, 2 * ((v[1] & 15) + 1));
	r->byte = byte_parent >> 2;
	return FW_CLOCK_COUNT;
}

static bool fw_clock_rate_matches(u64 a, u64 b)
{
	return (a > b ? a - b : b - a) <= 1000;
}

/* Zero is allowed only after successful physical-register decoding. */
static bool fw_clock_cache_matches(u64 cached, u64 decoded)
{
	return !cached || fw_clock_rate_matches(cached, decoded);
}
#endif

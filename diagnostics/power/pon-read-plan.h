/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUPERMAN_PON_READ_PLAN_H
#define SUPERMAN_PON_READ_PLAN_H

/* Gen1 offsets from Qualcomm's qpnp-power-on.c. Identity comes first;
 * newer subtypes have different offsets and must never use this plan.
 * No latch-clear, control, security, IRQ or watchdog registers are included.
 */
#define SUPERMAN_PON_BASE 0x800U
#define SUPERMAN_PON_IDENTITIES 2U
#define SUPERMAN_PON_REASONS 5U
#define SUPERMAN_PON_REGISTERS 7U

struct superman_pon_register {
	unsigned int offset;
	const char *name;
};

static const struct superman_pon_register superman_pon_allowlist[] = {
	{ 0x01, "REVISION2" },
	{ 0x05, "SUBTYPE" },
	{ 0x08, "PON_REASON1" },
	{ 0x0a, "WARM_RESET_REASON1" },
	{ 0x0b, "WARM_RESET_REASON2" },
	{ 0x0c, "POFF_REASON1" },
	{ 0x0d, "POFF_REASON2" },
};

struct superman_pon_snapshot {
	unsigned int value[SUPERMAN_PON_REGISTERS];
	int error[SUPERMAN_PON_REGISTERS];
	unsigned int reads;
	unsigned int gen1_verified;
	unsigned int complete;
};

typedef int (*superman_pon_reader)(void *, unsigned int, unsigned int *);

/* Called once at module initialization. The sysfs getter uses only this
 * cached result; it cannot cause another SPMI transaction.
 */
static int superman_pon_read_once(superman_pon_reader read_register, void *context,
				 struct superman_pon_snapshot *snapshot)
{
	unsigned int i, subtype;
	int first_error = 0;

	for (i = 0; i < SUPERMAN_PON_IDENTITIES; i++) {
		snapshot->error[i] = read_register(context,
			SUPERMAN_PON_BASE + superman_pon_allowlist[i].offset,
			&snapshot->value[i]);
		snapshot->reads++;
		if (snapshot->error[i] && !first_error)
			first_error = snapshot->error[i];
	}
	if (first_error)
		return first_error;
	if (snapshot->value[0] > 0xff)
		return -ENODEV;
	subtype = snapshot->value[1];
	if (subtype != 0x01 && subtype != 0x02 && subtype != 0x03)
		return -ENODEV;
	snapshot->gen1_verified = 1;

	for (; i < SUPERMAN_PON_REGISTERS; i++) {
		snapshot->error[i] = read_register(context,
			SUPERMAN_PON_BASE + superman_pon_allowlist[i].offset,
			&snapshot->value[i]);
		snapshot->reads++;
		if (snapshot->error[i] && !first_error)
			first_error = snapshot->error[i];
	}
	snapshot->complete = !first_error;
	return first_error;
}

#endif

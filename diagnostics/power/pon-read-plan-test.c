/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "pon-read-plan.h"

struct fake_bus {
	unsigned int calls, subtype, revision, fail_at;
};

static int read_fake(void *context, unsigned int address, unsigned int *value)
{
	struct fake_bus *bus = context;
	static const unsigned int permitted[] = {
		0x801, 0x805, 0x808, 0x80a, 0x80b, 0x80c, 0x80d,
	};
	assert(bus->calls < sizeof(permitted) / sizeof(permitted[0]));
	assert(address == permitted[bus->calls]);
	bus->calls++;
	if (bus->calls == bus->fail_at)
		return -EIO;
	*value = address == 0x805 ? bus->subtype :
		(address == 0x801 ? bus->revision : 0xff);
	return 0;
}

int main(void)
{
	struct superman_pon_snapshot snapshot;
	struct fake_bus bus;
	unsigned int subtype, fail, cases = 0;
	int ret;

	for (subtype = 0; subtype <= 0xff; subtype++) {
		memset(&snapshot, 0, sizeof(snapshot));
		bus = (struct fake_bus){ .subtype = subtype, .revision = 1 };
		ret = superman_pon_read_once(read_fake, &bus, &snapshot);
		if (subtype >= 1 && subtype <= 3) {
			assert(ret == 0 && snapshot.complete && snapshot.gen1_verified);
			assert(bus.calls == 7 && snapshot.reads == 7);
		} else {
			assert(ret == -ENODEV && !snapshot.gen1_verified && !snapshot.complete);
			assert(bus.calls == 2 && snapshot.reads == 2);
		}
		cases++;
	}
	for (fail = 1; fail <= 7; fail++) {
		memset(&snapshot, 0, sizeof(snapshot));
		bus = (struct fake_bus){ .subtype = 1, .revision = 1, .fail_at = fail };
		ret = superman_pon_read_once(read_fake, &bus, &snapshot);
		assert(ret == -EIO && !snapshot.complete && snapshot.error[fail - 1] == -EIO);
		assert(bus.calls == (fail <= 2 ? 2 : 7));
		assert(snapshot.gen1_verified == (fail > 2));
		cases++;
	}
	memset(&snapshot, 0, sizeof(snapshot));
	bus = (struct fake_bus){ .subtype = 1, .revision = 0x100 };
	assert(superman_pon_read_once(read_fake, &bus, &snapshot) == -ENODEV);
	assert(bus.calls == 2 && !snapshot.gen1_verified);
	cases++;
	printf("PASS %u cases: gen1-only reasons, identity failures reject, exact seven read addresses, partial errors retained\n", cases);
	return 0;
}

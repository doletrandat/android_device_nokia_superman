// SPDX-License-Identifier: Apache-2.0
#ifndef SUPERMAN_RIL_DATA_LINK_H
#define SUPERMAN_RIL_DATA_LINK_H
#include "data-call.h"
#include <net/if.h>
struct data_link {
    int fd, index, initial_mtu;
    short initial_flags;
    int flags_owned, mtu_owned, address_owned, mask_set;
    unsigned selected_mtu;
    uint32_t address, mask;
    char name[IFNAMSIZ];
};
/* Prepare reads only and requires the exact BAM channel0 to be down and empty. */
int data_link_prepare(struct data_link *link);
int data_link_apply(struct data_link *link, const struct data_lease *lease);
/* On failure keeps its ownership fields/socket for an explicit cleanup retry. */
int data_link_restore(struct data_link *link);
#endif

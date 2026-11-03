// SPDX-License-Identifier: Apache-2.0
// Touch only our empty, down BAM-DMUX channel; no DNS/global routing changes.
#include "data-link.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
static int channel0(const char *name) {
    char path[PATH_MAX], driver[PATH_MAX];
    unsigned port, type;
    FILE *f;
    if (!*name || strlen(name) >= IFNAMSIZ)
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", name);
    if (!realpath(path, driver) ||
        strcmp(strrchr(driver, '/') ? strrchr(driver, '/') + 1 : driver, "bam-dmux"))
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/dev_port", name);
    f = fopen(path, "re");
    if (!f)
        return 0;
    int n = fscanf(f, "%u", &port);
    fclose(f);
    if (n != 1 || port != 0)
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/type", name);
    f = fopen(path, "re");
    if (!f)
        return 0;
    n = fscanf(f, "%u", &type);
    fclose(f);
    return n == 1 && type == ARPHRD_RAWIP;
}
static int query(struct data_link *link, unsigned long operation, struct ifreq *request) {
    memset(request, 0, sizeof(*request));
    memcpy(request->ifr_name, link->name, strlen(link->name) + 1);
    return ioctl(link->fd, operation, request) ? -errno : 0;
}
static int same_device(struct data_link *link) {
    struct ifreq request;
    if (link->fd < 0 || !channel0(link->name))
        return -ENODEV;
    int rc = query(link, SIOCGIFINDEX, &request);
    return rc ? rc : request.ifr_ifindex == link->index ? 0 : -ENODEV;
}
static int addresses(struct data_link *link, int expect_owned) {
    struct ifaddrs *list;
    if (getifaddrs(&list))
        return -errno;
    int rc = 0;
    unsigned found = 0;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || strcmp(p->ifa_name, link->name))
            continue;
        uint32_t address = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        if (!address)
            continue;
        found++;
        if (!expect_owned || address != link->address || !p->ifa_netmask ||
            (link->mask_set &&
             ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr) != link->mask)) {
            rc = -EBUSY;
            break;
        }
    }
    freeifaddrs(list);
    return rc ? rc : expect_owned && found != 1 ? -EBUSY : 0;
}
int data_link_prepare(struct data_link *link) {
    if (!link)
        return -EINVAL;
    if (link->fd >= 0)
        return -EBUSY;
    struct data_link selected = {.fd = -1};
    DIR *dir = opendir("/sys/class/net");
    if (!dir)
        return -errno;
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(dir)))
        if (entry->d_name[0] != '.' && channel0(entry->d_name)) {
            count++;
            if (count == 1)
                memcpy(selected.name, entry->d_name, strlen(entry->d_name) + 1);
        }
    closedir(dir);
    if (count != 1)
        return count ? -EEXIST : -ENODEV;
    selected.fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (selected.fd < 0)
        return -errno;
    struct ifreq request;
    int rc = query(&selected, SIOCGIFINDEX, &request);
    if (rc)
        goto fail;
    selected.index = request.ifr_ifindex;
    rc = query(&selected, SIOCGIFFLAGS, &request);
    if (rc)
        goto fail;
    selected.initial_flags = request.ifr_flags;
    if (selected.initial_flags & IFF_UP) {
        rc = -EBUSY;
        goto fail;
    }
    rc = query(&selected, SIOCGIFMTU, &request);
    if (rc)
        goto fail;
    selected.initial_mtu = request.ifr_mtu;
    if (selected.initial_mtu < 576 || selected.initial_mtu > 2040) {
        rc = -ERANGE;
        goto fail;
    }
    rc = addresses(&selected, 0);
    if (rc)
        goto fail;
    *link = selected;
    return 0;
fail:
    close(selected.fd);
    return rc;
}
static int set_mtu(struct data_link *link, unsigned mtu) {
    struct ifreq request = {0};
    memcpy(request.ifr_name, link->name, strlen(link->name) + 1);
    request.ifr_mtu = mtu;
    return ioctl(link->fd, SIOCSIFMTU, &request) ? -errno : 0;
}
static int set_flags(struct data_link *link, short flags) {
    struct ifreq request = {0};
    memcpy(request.ifr_name, link->name, strlen(link->name) + 1);
    request.ifr_flags = flags;
    return ioctl(link->fd, SIOCSIFFLAGS, &request) ? -errno : 0;
}
static int set_address(struct data_link *link, unsigned long operation, uint32_t address) {
    struct ifreq request = {0};
    memcpy(request.ifr_name, link->name, strlen(link->name) + 1);
    struct sockaddr_in *in = (struct sockaddr_in *)&request.ifr_addr;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = htonl(address);
    return ioctl(link->fd, operation, &request) ? -errno : 0;
}
int data_link_apply(struct data_link *link, const struct data_lease *lease) {
    if (!link || !lease || lease->mtu < 576 || lease->mtu > 1500 || !lease->address || !lease->mask)
        return -EINVAL;
    int rc = same_device(link);
    if (rc)
        return rc;
    if (link->flags_owned || link->mtu_owned || link->address_owned)
        return -EBUSY;
    struct ifreq request;
    rc = query(link, SIOCGIFFLAGS, &request);
    if (rc)
        return rc;
    if (request.ifr_flags != link->initial_flags)
        return -EBUSY;
    rc = query(link, SIOCGIFMTU, &request);
    if (rc)
        return rc;
    if (request.ifr_mtu != link->initial_mtu)
        return -EBUSY;
    rc = addresses(link, 0);
    if (rc)
        return rc;
    link->selected_mtu = lease->mtu;
    link->address = lease->address;
    link->mask = lease->mask;
    if (lease->mtu != (unsigned)link->initial_mtu) {
        rc = set_mtu(link, lease->mtu);
        if (rc)
            return rc;
        link->mtu_owned = 1;
    }
    rc = set_flags(link, link->initial_flags | IFF_UP);
    if (rc)
        return rc;
    link->flags_owned = 1;
    rc = set_address(link, SIOCSIFADDR, lease->address);
    if (rc)
        return rc;
    link->address_owned = 1;
    rc = set_address(link, SIOCSIFNETMASK, lease->mask);
    if (rc)
        return rc;
    link->mask_set = 1;
    return addresses(link, 1);
}
int data_link_restore(struct data_link *link) {
    if (!link)
        return -EINVAL;
    if (link->fd < 0)
        return 0;
    if (!link->name[0])
        return -EINVAL;
    if (!link->flags_owned && !link->mtu_owned && !link->address_owned) {
        close(link->fd);
        link->fd = -1;
        return 0;
    }
    int rc = same_device(link);
    if (rc)
        return rc;
    if (link->address_owned) {
        rc = addresses(link, 1);
        if (rc)
            return rc;
    } else {
        rc = addresses(link, 0);
        if (rc)
            return rc;
    }
    struct ifreq request;
    if (link->mtu_owned) {
        rc = query(link, SIOCGIFMTU, &request);
        if (rc)
            return rc;
        if (request.ifr_mtu != (int)link->selected_mtu)
            return -EBUSY;
    }
    if (link->address_owned) {
        rc = set_address(link, SIOCSIFADDR, 0);
        if (rc)
            return rc;
        link->address_owned = 0;
        link->mask_set = 0;
    }
    if (link->mtu_owned) {
        rc = set_mtu(link, link->initial_mtu);
        if (rc)
            return rc;
        link->mtu_owned = 0;
    }
    if (link->flags_owned) {
        rc = query(link, SIOCGIFFLAGS, &request);
        if (rc)
            return rc;
        rc = set_flags(link, (request.ifr_flags & ~IFF_UP) | (link->initial_flags & IFF_UP));
        if (rc)
            return rc;
        link->flags_owned = 0;
    }
    close(link->fd);
    link->fd = -1;
    return 0;
}

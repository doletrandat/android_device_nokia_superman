// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
static int descriptors, index_value = 7, mtu = 1500, flags = 0x90, ioctls, fail_ioctl, dumps,
                        fail_dump, setters;
static uint32_t address, mask, foreign_address;
static int fake_socket(int family, int type, int protocol) {
    assert(family == AF_INET && (type & SOCK_DGRAM) && !protocol && !descriptors);
    descriptors++;
    return 42;
}
static int fake_close(int fd) {
    assert(fd == 42 && descriptors == 1);
    descriptors--;
    return 0;
}
static int fake_ioctl(int fd, unsigned long op, ...) {
    assert(fd == 42 && descriptors == 1);
    va_list args;
    va_start(args, op);
    struct ifreq *r = va_arg(args, struct ifreq *);
    va_end(args);
    assert(!strcmp(r->ifr_name, "wwan0"));
    if (++ioctls == fail_ioctl) {
        errno = EIO;
        return -1;
    }
    switch (op) {
    case SIOCGIFINDEX:
        r->ifr_ifindex = index_value;
        break;
    case SIOCGIFFLAGS:
        r->ifr_flags = flags | ((flags & IFF_UP) ? IFF_RUNNING : 0);
        break;
    case SIOCGIFMTU:
        r->ifr_mtu = mtu;
        break;
    case SIOCSIFMTU:
        setters++;
        mtu = r->ifr_mtu;
        break;
    case SIOCSIFFLAGS:
        setters++;
        flags = r->ifr_flags & ~IFF_RUNNING;
        break;
    case SIOCSIFADDR:
        setters++;
        address = ntohl(((struct sockaddr_in *)&r->ifr_addr)->sin_addr.s_addr);
        mask = address ? 0xff000000 : 0;
        break;
    case SIOCSIFNETMASK:
        assert(address);
        setters++;
        mask = ntohl(((struct sockaddr_in *)&r->ifr_netmask)->sin_addr.s_addr);
        break;
    default:
        assert(!"Unexpected interface/global routing operation");
    }
    return 0;
}
static struct ifaddrs interfaces[3];
static struct sockaddr_in ips[3], masks[3];
static int fake_getifaddrs(struct ifaddrs **out) {
    if (++dumps == fail_dump) {
        errno = EIO;
        return -1;
    }
    memset(interfaces, 0, sizeof(interfaces));
    memset(ips, 0, sizeof(ips));
    memset(masks, 0, sizeof(masks));
    uint32_t values[] = {address, foreign_address, 0xc0a80102},
             prefixes[] = {mask, 0xffffff00, 0xffffff00};
    for (unsigned i = 0; i < 3; i++) {
        interfaces[i].ifa_name = i == 2 ? "wlan0" : "wwan0";
        interfaces[i].ifa_next = i < 2 ? &interfaces[i + 1] : NULL;
        ips[i].sin_family = AF_INET;
        ips[i].sin_addr.s_addr = htonl(values[i]);
        masks[i].sin_family = AF_INET;
        masks[i].sin_addr.s_addr = htonl(prefixes[i]);
        interfaces[i].ifa_addr = (struct sockaddr *)&ips[i];
        interfaces[i].ifa_netmask = (struct sockaddr *)&masks[i];
    }
    *out = interfaces;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *p) { assert(p == interfaces); }
#define socket fake_socket
#define close fake_close
#define ioctl fake_ioctl
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#include DATA_LINK_SOURCE
static void reset(void) {
    assert(!descriptors);
    index_value = 7;
    mtu = 1500;
    flags = 0x90;
    address = mask = foreign_address = 0;
    ioctls = fail_ioctl = dumps = fail_dump = setters = 0;
}
static void clean(void) {
    assert(!descriptors && mtu == 1500 && flags == 0x90 && !address && !mask);
}
int main(void) {
    struct data_lease lease = {.address = 0x0a000002,
                               .mask = 0xfffffffc,
                               .gateway = 0x0a000001,
                               .prefix = 30,
                               .mtu = 1428};
    struct data_link link = {.fd = -1};
    reset();
    assert(!data_link_prepare(&link) && descriptors == 1 && !setters);
    assert(data_link_prepare(&link) == -EBUSY);
    assert(!data_link_restore(&link) && !setters);
    clean();
    reset();
    flags |= IFF_UP;
    assert(data_link_prepare(&link) == -EBUSY && !descriptors && !setters);
    flags = 0x90;
    address = 0x0a000009;
    mask = 0xffffff00;
    assert(data_link_prepare(&link) == -EBUSY && !descriptors && !setters);
    address = mask = 0;
    reset();
    assert(!data_link_prepare(&link));
    ioctls = 0;
    assert(!data_link_apply(&link, &lease));
    int apply_calls = ioctls;
    assert(address == lease.address && mask == lease.mask && mtu == 1428 && (flags & IFF_UP));
    ioctls = 0;
    assert(!data_link_restore(&link));
    int restore_calls = ioctls;
    clean();
    for (int failure = 1; failure <= apply_calls; failure++) {
        reset();
        assert(!data_link_prepare(&link));
        ioctls = 0;
        fail_ioctl = failure;
        assert(data_link_apply(&link, &lease) == -EIO);
        fail_ioctl = 0;
        assert(!data_link_restore(&link));
        clean();
    }
    for (int failure = 1; failure <= restore_calls; failure++) {
        reset();
        assert(!data_link_prepare(&link));
        assert(!data_link_apply(&link, &lease));
        ioctls = 0;
        fail_ioctl = failure;
        assert(data_link_restore(&link) == -EIO && descriptors == 1 && link.fd == 42);
        fail_ioctl = 0;
        assert(!data_link_restore(&link));
        clean();
    }
    for (int failure = 1; failure <= 2; failure++) {
        reset();
        assert(!data_link_prepare(&link));
        dumps = 0;
        fail_dump = failure;
        assert(data_link_apply(&link, &lease) == -EIO);
        fail_dump = 0;
        assert(!data_link_restore(&link));
        clean();
    }
    reset();
    assert(!data_link_prepare(&link));
    mtu = 1490;
    assert(data_link_apply(&link, &lease) == -EBUSY && !setters);
    assert(!data_link_restore(&link) && mtu == 1490 && !address && !(flags & IFF_UP));
    mtu = 1500;
    reset();
    assert(!data_link_prepare(&link));
    assert(!data_link_apply(&link, &lease));
    foreign_address = 0x0a000003;
    int before = setters;
    assert(data_link_restore(&link) == -EBUSY && setters == before && descriptors == 1);
    foreign_address = 0;
    assert(!data_link_restore(&link));
    clean();
    reset();
    assert(!data_link_prepare(&link));
    assert(!data_link_apply(&link, &lease));
    mtu = 1400;
    before = setters;
    assert(data_link_restore(&link) == -EBUSY && setters == before);
    mtu = 1428;
    assert(!data_link_restore(&link));
    clean();
    reset();
    assert(!data_link_prepare(&link));
    assert(!data_link_apply(&link, &lease));
    mask = 0xffffff00;
    before = setters;
    assert(data_link_restore(&link) == -EBUSY && setters == before);
    mask = lease.mask;
    assert(!data_link_restore(&link));
    clean();
    reset();
    assert(!data_link_prepare(&link));
    assert(!data_link_apply(&link, &lease));
    index_value = 8;
    before = setters;
    assert(data_link_restore(&link) == -ENODEV && setters == before);
    index_value = 7;
    assert(!data_link_restore(&link));
    clean();
    reset();
    assert(!data_link_prepare(&link));
    assert(!data_link_apply(&link, &lease));
    flags |= IFF_ALLMULTI;
    assert(!data_link_restore(&link) && flags == (0x90 | IFF_ALLMULTI) && !address && mtu == 1500 &&
           !descriptors);
    flags = 0x90;
    printf("PASS actual owned-link functions: read-only preparation, %d apply/%d restore failure "
           "points, cleanup retries, foreign address/mask/MTU/index refusal and unrelated flag "
           "preservation\n",
           apply_calls, restore_calls);
}

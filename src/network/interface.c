#include "network/network.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

static int query(const char *name, unsigned long request, struct ifreq *ifr)
{
    memset(ifr, 0, sizeof(*ifr));
    snprintf(ifr->ifr_name, IFNAMSIZ, "%s", name);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int rc = ioctl(fd, request, ifr);
    close(fd);
    return rc;
}

bool sn_interface(const struct config *cfg, bool require_up)
{
    struct ifreq ifr;
    return !query(cfg->interface, SIOCGIFFLAGS, &ifr) && (!require_up || (ifr.ifr_flags & IFF_UP));
}

int sn_address(const char *name, char out[16])
{
    struct ifreq ifr;
    if (query(name, SIOCGIFADDR, &ifr)) {
        return -1;
    }
    struct sockaddr_in *address = (struct sockaddr_in *)&ifr.ifr_addr;
    return inet_ntop(AF_INET, &address->sin_addr, out, 16) ? 0 : -1;
}

int sn_net_configure(const struct config *cfg)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = -1;
    if (fd < 0) {
        return -1;
    }
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", cfg->interface);
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, cfg->address, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr)) {
        goto done;
    }
    addr->sin_addr.s_addr = htonl(UINT32_MAX << (32 - cfg->prefix));
    if (ioctl(fd, SIOCSIFNETMASK, &ifr)) {
        goto done;
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr)) {
        goto done;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr)) {
        goto done;
    }
    rc = 0;
done:
    if (rc) {
        sn_log(LOG_ERR, "configure %s: %s", cfg->interface, strerror(errno));
    }
    close(fd);
    return rc;
}

int sn_net_down(const struct config *cfg)
{
    struct ifreq ifr;
    if (query(cfg->interface, SIOCGIFFLAGS, &ifr)) {
        return errno == ENODEV || errno == ENXIO ? 0 : -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    ifr.ifr_flags &= ~IFF_UP;
    int rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    close(fd);
    return rc;
}

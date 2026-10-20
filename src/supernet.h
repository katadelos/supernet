#ifndef SUPERNET_H
#define SUPERNET_H

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SN_HOME SN_SDK_ROOT "/etc/supernet"
#define SN_SOURCE_BIN SN_SDK_ROOT "/usr/bin/"
#define SN_CONFIG SN_HOME "/config"
#define SN_LOG "/dev/supernet-sdk/supernet.log"
#define SN_RUN "/dev/supernet-sdk"
#define SN_BIN SN_RUN "/bin/"
#ifdef SN_KOBO
#define SN_KEYS "/mnt/onboard/supernet"
#else
#define SN_KEYS "/mnt/us/supernet"
#endif
#define SN_HOSTKEY SN_KEYS "/host_ed25519"
#define SN_SOCKET SN_RUN "/control"

struct config {
    char interface[IFNAMSIZ];
    char address[16];
    unsigned prefix;
    unsigned port;
    unsigned timeout;
    bool wifi;
    bool usb;
};

#endif

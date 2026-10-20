#include "network/network.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <arpa/inet.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>

int sn_wifi_address(char interface[IFNAMSIZ], char address[16])
{
    glob_t paths = {0};
    glob("/sys/class/net/*/wireless", 0, NULL, &paths);
    int rc = -1;
    interface[0] = address[0] = 0;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        const char *begin = paths.gl_pathv[i] + strlen("/sys/class/net/");
        const char *end = strchr(begin, '/');
        if (!end || end - begin >= IFNAMSIZ) {
            continue;
        }
        char name[IFNAMSIZ];
        memcpy(name, begin, (size_t)(end - begin));
        name[end - begin] = 0;
        struct config probe = {0};
        strcpy(probe.interface, name);
        if (!sn_interface(&probe, true)) {
            continue;
        }
        char carrier_path[128];
        snprintf(carrier_path, sizeof(carrier_path), "/sys/class/net/%s/carrier", name);
        FILE *carrier = fopen(carrier_path, "r");
        int connected = carrier ? fgetc(carrier) : EOF;
        if (carrier) {
            fclose(carrier);
        }
        if (connected != '1' || sn_address(name, address)) {
            continue;
        }
        struct in_addr ipv4;
        if (inet_pton(AF_INET, address, &ipv4) != 1) {
            continue;
        }
        uint32_t host = ntohl(ipv4.s_addr);
        if (!(host >> 24) || (host >> 24) == 127 || (host >> 24) >= 224) {
            continue;
        }
        strcpy(interface, name);
        rc = 0;
        break;
    }
    globfree(&paths);
    if (rc) {
        interface[0] = address[0] = 0;
    }
    return rc;
}

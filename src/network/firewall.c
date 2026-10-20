#include "network/network.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool sn_firewall_pending(void)
{
    return !access(SN_RUN "/firewall", F_OK);
}

int sn_firewall_set(const struct config *cfg, const char *interface, const char *address, bool add)
{
#ifdef SN_KOBO
    (void)cfg;
    (void)interface;
    (void)address;
    (void)add;
    return 0; /* Stock Nickel does not install an inbound IP firewall. */
#else
    char port[8], token[64], old_interface[IFNAMSIZ], old_address[16];
    unsigned old_port;
    if (add) {
        snprintf(port, sizeof(port), "%u", cfg->port);
        snprintf(token, sizeof(token), "supernet-sdk-%ld", (long)getpid());
        int fd =
            open(SN_RUN "/firewall", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            return -1;
        }
        int rc =
            dprintf(fd, "%s %s %u %s\n", interface, address, cfg->port, token) < 0 || fsync(fd);
        close(fd);
        if (rc) {
            return -1;
        }
    } else {
        FILE *file = fopen(SN_RUN "/firewall", "r");
        if (!file) {
            return errno == ENOENT ? 0 : -1;
        }
        int fields =
            fscanf(file, "%15s %15s %u %63s", old_interface, old_address, &old_port, token);
        fclose(file);
        struct in_addr ip;
        if (fields != 4 || !old_port || old_port > 65535 ||
            strspn(old_interface,
                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                strlen(old_interface) ||
            inet_pton(AF_INET, old_address, &ip) != 1 || strncmp(token, "supernet-sdk-", 13) ||
            strspn(token + 13, "0123456789") != strlen(token + 13)) {
            return -1;
        }
        interface = old_interface;
        address = old_address;
        snprintf(port, sizeof(port), "%u", old_port);
    }
    char *args[] = {"/usr/sbin/iptables",
                    add ? "-I" : "-C",
                    "INPUT",
                    "-i",
                    (char *)interface,
                    "-d",
                    (char *)address,
                    "-p",
                    "tcp",
                    "--dport",
                    port,
                    "-m",
                    "comment",
                    "--comment",
                    token,
                    "-j",
                    "ACCEPT",
                    NULL};
    int rc = sn_run(args, 3000);
    if (add) {
        return rc;
    }
    if (!rc) {
        args[1] = "-D";
        rc = sn_run(args, 3000);
    } else if (rc == 1) {
        rc = 0; /* Journaled intent never reached iptables. */
    }
    if (!rc && unlink(SN_RUN "/firewall")) {
        rc = -1;
    }
    return rc;
#endif
}

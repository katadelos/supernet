#include "network/network.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#ifndef SN_KOBO
enum firewall_state { CHAIN_PRESENT = 1, JUMP_PRESENT = 2 };

/* Firmware iptables 1.3 has neither -C nor -S, and older Kindle kernels lack
 * the comment match. A private chain identifies our rule without extensions.
 * Read back partial changes as well as successful starts for cleanup. */
static int firewall_state(const char *token)
{
    char path[] = SN_RUN "/firewall-check-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    if (unlink(path) || fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        close(fd);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (!pid) {
        if (dup2(fd, STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(fd);
        execl("/usr/sbin/iptables-save", "iptables-save", "-t", "filter", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    uint64_t end = sn_now() + 3000;
    pid_t result;
    do {
        result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno != EINTR)) {
            break;
        }
        sn_sleep(25);
    } while (sn_now() < end);
    if (result != pid) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (result != pid || !WIFEXITED(status) || WEXITSTATUS(status) || lseek(fd, 0, SEEK_SET)) {
        sn_log(LOG_ERR, "could not read the Kindle firewall rules");
        close(fd);
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return -1;
    }
    char line[4096];
    int present = 0;
    size_t length = strlen(token);
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            present = -1;
            break;
        }
        if (*line == ':' && !strncmp(line + 1, token, length) && line[length + 1] == ' ') {
            present |= CHAIN_PRESENT;
        }
        char *jump = strstr(line, " -j ");
        if (!strncmp(line, "-A INPUT ", 9) && jump) {
            jump += strlen(" -j ");
            if (!strncmp(jump, token, length) && strchr(" \n", jump[length])) {
                present |= JUMP_PRESENT;
            }
        }
    }
    if (ferror(file)) {
        present = -1;
    }
    fclose(file);
    return present;
}
#endif

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
        /* Never reserve a chain that already belongs to another controller. */
        if (firewall_state(token) != 0) {
            sn_log(LOG_ERR, "Supernet firewall chain is unavailable");
            return -1;
        }
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
            !token[13] || strlen(token) > 28 ||
            strspn(token + 13, "0123456789") != strlen(token + 13)) {
            return -1;
        }
        interface = old_interface;
        address = old_address;
        snprintf(port, sizeof(port), "%u", old_port);
    }
    char *args[] = {"/usr/sbin/iptables",
                    add ? "-I" : "-D",
                    "INPUT",
                    "-i",
                    (char *)interface,
                    "-d",
                    (char *)address,
                    "-p",
                    "tcp",
                    "--dport",
                    port,
                    "-j",
                    token,
                    NULL};
    if (add) {
        char *create[] = {"/usr/sbin/iptables", "-N", token, NULL};
        char *accept[] = {"/usr/sbin/iptables", "-A", token, "-j", "ACCEPT", NULL};
        if (sn_run(create, 3000) || sn_run(accept, 3000)) {
            return -1;
        }
        return sn_run(args, 3000);
    }
    int present = firewall_state(token);
    if (present < 0) {
        return -1;
    }
    int rc = (present & JUMP_PRESENT) ? sn_run(args, 3000) : 0;
    if (!rc && (present & CHAIN_PRESENT)) {
        char *flush[] = {"/usr/sbin/iptables", "-F", token, NULL};
        char *remove[] = {"/usr/sbin/iptables", "-X", token, NULL};
        rc = sn_run(flush, 3000) || sn_run(remove, 3000);
    }
    if (!rc && unlink(SN_RUN "/firewall")) {
        rc = -1;
    }
    return rc;
#endif
}

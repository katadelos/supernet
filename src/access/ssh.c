#include "access/access.h"
#include "network/network.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

bool sn_keys_ready(void)
{
    int fd = open(SN_KEYS "/authorized_keys", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return false;
    }
    char line[4096];
    bool found = false, valid = true;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            valid = false;
            break;
        }
        char *save, *kind = strtok_r(line, " \t\r\n", &save);
        if (!kind || *kind == '#') {
            continue;
        }
        char *key = strtok_r(NULL, " \t\r\n", &save);
        if ((strcmp(kind, "ssh-ed25519") && strcmp(kind, "ssh-rsa") &&
             strncmp(kind, "ecdsa-sha2-nistp", 16)) ||
            !key || strlen(key) < 32 ||
            strspn(key, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
                strlen(key)) {
            valid = false;
            break;
        }
        found = true;
    }
    if (ferror(file)) {
        valid = false;
    }
    fclose(file);
    return found && valid;
}

int sn_keys_prepare(void)
{
    struct stat st;
    if (lstat(SN_KEYS "/authorized_keys", &st) || !S_ISREG(st.st_mode) || !st.st_size) {
        sn_log(LOG_ERR, "Add SSH public keys to %s", SN_KEYS "/authorized_keys");
        return -1;
    }
    /* Host keys persist on userstore; Dropbear reads their explicit path. */
    if (lstat(SN_HOSTKEY, &st)) {
        if (errno != ENOENT) {
            return -1;
        }
        char *args[] = {SN_BIN "dropbearkey", "-t", "ed25519", "-f", SN_HOSTKEY, NULL};
        if (sn_run(args, 10000) || lstat(SN_HOSTKEY, &st)) {
            return -1;
        }
    }
    if (!S_ISREG(st.st_mode) || !st.st_size) {
        sn_log(LOG_ERR, "Invalid SSH host key: %s", SN_HOSTKEY);
        return -1;
    }
    /* Dropbear checks authorized_keys and its directory permissions. FAT
     * cannot provide those permissions, so authenticate from a private copy.
     * TODO: Patch Dropbear to accept Kindle userstore permissions; its FAT
     * mode bits do not represent a security boundary in that environment. */
    return sn_copy(SN_KEYS "/authorized_keys", SN_RUN "/authorized_keys", 0600);
}

bool sn_package_busy(void)
{
    const char *path = "/tmp/eink-opkg.lock";
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return errno != ENOENT;
    }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    bool busy = fcntl(fd, F_GETLK, &lock) || lock.l_type != F_UNLCK;
    close(fd);
    return busy;
}

static bool port_busy(unsigned port)
{
    const char *paths[] = {"/proc/net/tcp", "/proc/net/tcp6"};
    for (unsigned i = 0; i < 2; i++) {
        FILE *file = fopen(paths[i], "r");
        if (!file) {
            if (!i) {
                return true;
            }
            continue;
        }
        char line[512], local[80], remote[80];
        unsigned state;
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, " %*u: %79s %79s %x", local, remote, &state) != 3 || state != 10) {
                continue;
            }
            char *colon = strrchr(local, ':');
            if (colon && strtoul(colon + 1, NULL, 16) == port) {
                fclose(file);
                return true;
            }
        }
        fclose(file);
    }
    return false;
}

const char *sn_access_problem(const struct config *cfg, bool ssh)
{
    if (sn_package_busy()) {
        return "A package operation is running. Wait for it to finish.";
    }
    if (!sn_keys_ready()) {
        return "Add an OpenSSH public key to the authorized_keys file before starting.";
    }
    if (!ssh && port_busy(cfg->port)) {
        return "Another SSH service is using this port. Stop it explicitly or choose a different "
               "port.";
    }
    if (!ssh && (sn_ssh_processes(0) != 0 || sn_firewall_pending())) {
        return "Supernet has unfinished SSH resources. Stop access to clean them up first.";
    }
    return "";
}

int sn_access_check(const struct config *cfg, bool ssh)
{
    const char *error = sn_access_problem(cfg, ssh);
    if (*error) {
        sn_log(LOG_ERR, "%s", error);
        return -1;
    }
    if (cfg->usb) {
        if (sn_adb_running()) {
            sn_log(LOG_ERR, "Stop ADB before starting USB SSH");
            return -1;
        }
        char detail[256] = "";
        enum sn_usb_state state = sn_usb_state(cfg, detail, sizeof(detail));
        if (strcmp(cfg->interface, "usb0") || !sn_usb_can_start(state)) {
            sn_log(LOG_ERR, "%s", *detail ? detail : "Only the usb0 interface is supported");
            return -1;
        }
    }
    if (cfg->wifi) {
        char interface[IFNAMSIZ], address[16];
        if (sn_wifi_address(interface, address)) {
            sn_log(LOG_ERR, "Connect Wi-Fi in the stock reader before starting Wi-Fi access");
            return -1;
        }
    }
    return 0;
}

int sn_ssh_processes(int signal_number)
{
    return sn_processes(SN_BIN "dropbearmulti", 0, signal_number);
}

int sn_ssh_recover(void)
{
    if (sn_processes_stop(SN_BIN "dropbearmulti", 0)) {
        return -1;
    }
    return sn_firewall_set(NULL, NULL, NULL, false);
}

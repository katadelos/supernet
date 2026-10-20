#include "control/control.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

static bool service_ready(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, SN_SOCKET);
    bool ready = !connect(fd, (struct sockaddr *)&address, sizeof(address));
    close(fd);
    return ready;
}

int sn_stage(void)
{
    sn_log(LOG_INFO, "staging runtime in %s", SN_RUN);
    if (sn_mkdir(SN_RUN, 0700) || sn_mkdir(SN_RUN "/bin", 0700)) {
        return -1;
    }
    int lock = open(SN_RUN "/stage.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) {
        sn_log(LOG_ERR, "open runtime staging lock: %s", strerror(errno));
        return -1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        sn_log(LOG_ERR, "lock runtime staging: %s", strerror(errno));
        close(lock);
        return -1;
    }
    int rc = -1;
    if (service_ready()) {
        rc = 0;
        goto done;
    }
    if (sn_copy(SN_SOURCE_BIN "supernet", SN_BIN "supernet", 0700) ||
        sn_copy(SN_SDK_ROOT "/usr/sbin/dropbear", SN_BIN "dropbearmulti", 0700)) {
        goto done;
    }
    const char *applets[] = {"dropbear", "dropbearkey", "dbclient", "scp", NULL};
    for (unsigned i = 0; applets[i]; i++) {
        char path[128];
        snprintf(path, sizeof(path), SN_BIN "%s", applets[i]);
        unlink(path);
        if (symlink("dropbearmulti", path)) {
            sn_log(LOG_ERR, "create %s: %s", path, strerror(errno));
            goto done;
        }
    }
    rc = 0;
done:
    close(lock);
    return rc;
}

int sn_bootstrap(void)
{
    if (service_ready()) {
        return 0;
    }
    sn_log(LOG_INFO, "starting control service");
    if (sn_stage()) {
        return 1;
    }
    char *args[] = {SN_BIN "supernet", "daemon", NULL};
    if (sn_run(args, 3000)) {
        return 1;
    }
    uint64_t until = sn_now() + 10000;
    while (sn_now() < until) {
        if (service_ready()) {
            return 0;
        }
        sn_sleep(50);
    }
    sn_log(LOG_ERR, "control service did not start");
    return 1;
}

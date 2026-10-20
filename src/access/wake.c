#include "access/access.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static bool awake_requested;
bool sn_awake_requested(void)
{
    return awake_requested;
}

bool sn_wake_available(void)
{
    return !access("/sys/power/wake_lock", W_OK) && !access("/sys/power/wake_unlock", W_OK);
}

int sn_awake_set(bool enabled)
{
    if (enabled && !sn_wake_available()) {
        return -1;
    }
    awake_requested = enabled;
    sn_wake_update(enabled);
    if (enabled && !sn_wake_held()) {
        awake_requested = false;
        return -1;
    }
    return 0;
}

static bool wake_held;
static uint64_t wake_until, next_wake;
bool sn_wake_held(void)
{
    return wake_held && sn_now() < wake_until;
}

void sn_wake_update(bool active)
{
    char name[64], value[96];
    snprintf(name, sizeof(name), "supernet-%ld", (long)getpid());
    if (!active) {
        if (wake_held) {
            int fd = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t ignored = write(fd, name, strlen(name));
                (void)ignored;
                close(fd);
            }
        }
        wake_held = false;
        next_wake = 0;
        return;
    }
    if (sn_now() < next_wake) {
        return;
    }
    next_wake = sn_now() + 15000;
    snprintf(value, sizeof(value), "%s 45000000000\n", name);
    int fd = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        sn_log(LOG_WARNING, "cannot keep active SSH session awake: %s", strerror(errno));
        return;
    }
    if (write(fd, value, strlen(value)) == (ssize_t)strlen(value)) {
        wake_held = true;
        wake_until = sn_now() + 45000;
    }
    close(fd);
#ifndef SN_KOBO
    char *args[] = {"/usr/bin/lipc-set-prop",
                    "com.lab126.powerd",
                    "touchScreenSaverTimeout",
                    "1",
                    NULL};
    sn_run(args, 3000);
#endif
}

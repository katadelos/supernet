#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

bool sn_usb_owned(const struct config *cfg)
{
    struct stat st;
    char actual[128], expected[128];
    if (lstat(SN_RUN, &st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0077) ||
        sn_read_text(SN_RUN "/network", actual, sizeof(actual))) {
        return false;
    }
    snprintf(expected, sizeof(expected), "%s %s %u", cfg->interface, cfg->address, cfg->prefix);
    return !strcmp(actual, expected);
}

int sn_usb_mark_owned(const struct config *cfg)
{
    char text[128];
    snprintf(text, sizeof(text), "%s %s %u\n", cfg->interface, cfg->address, cfg->prefix);
    return sn_write_text(SN_RUN "/network", text);
}

int sn_usb_recover(void)
{
    struct stat st;
    if (lstat(SN_RUN, &st)) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0077)) {
        sn_log(LOG_ERR, "unsafe USB ownership directory; refusing cleanup");
        return -1;
    }
    int fd = open(SN_RUN "/network", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        return 0;
    }
    char text[128] = "", expected[128];
    struct config cfg = {0};
    bool valid = false;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && !st.st_uid && !(st.st_mode & 0077) &&
        st.st_size > 0 && st.st_size < (off_t)sizeof(text)) {
        ssize_t count;
        do {
            count = read(fd, text, sizeof(text) - 1);
        } while (count < 0 && errno == EINTR);
        /* Widths leave room for terminators and prevent numeric overflow. */
        if (count == st.st_size &&
            sscanf(text, "%15s %15s %2u", cfg.interface, cfg.address, &cfg.prefix) == 3 &&
            sn_config_network_valid(&cfg)) {
            int length = snprintf(expected,
                                  sizeof(expected),
                                  "%s %s %u\n",
                                  cfg.interface,
                                  cfg.address,
                                  cfg.prefix);
            valid = count == length && !memcmp(text, expected, (size_t)length);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (!valid) {
        sn_log(LOG_ERR, "invalid USB ownership journal; refusing cleanup");
        return -1;
    }
    return sn_usb_stop(&cfg);
}

bool sn_usb_can_start(enum sn_usb_state state)
{
    return state == SN_USB_IDLE || state == SN_USB_OWNED || state == SN_USB_RETRYABLE;
}

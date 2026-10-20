#include "platform/kobo_lock.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define USB_LOCK "/tmp/kobo-sdk-usbnet.lock"
#define OWNER_LOCK SN_RUN "/usb.lock"

static bool own_lock(void)
{
    struct stat st;
    char target[sizeof(OWNER_LOCK)];
    if (lstat(USB_LOCK, &st) || !S_ISLNK(st.st_mode) || st.st_uid) {
        return false;
    }
    ssize_t length = readlink(USB_LOCK, target, sizeof(target));
    return length == sizeof(OWNER_LOCK) - 1 && !memcmp(target, OWNER_LOCK, sizeof(OWNER_LOCK) - 1);
}

static int open_lock(bool create)
{
    struct stat st;
    if (lstat(SN_RUN, &st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0077)) {
        return -1;
    }
    int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    int fd = open(OWNER_LOCK, flags | (create ? O_CREAT : 0), 0600);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0077) ||
        st.st_nlink != 1 || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool sn_kobo_locked(void)
{
    struct stat st;
    if (lstat(USB_LOCK, &st)) {
        return errno != ENOENT;
    }
    if (!own_lock()) {
        return true;
    }
    int fd = open_lock(false);
    if (fd < 0) {
        return true;
    }
    close(fd);
    return false;
}

int sn_kobo_lock(void)
{
    int fd = open_lock(true);
    if (fd < 0) {
        sn_log(LOG_ERR, "USB controller is busy or its private lock is unavailable");
        return -1;
    }
    /* Publishing a symlink is atomic and excludes the bootstrap's mkdir lock.
     * The persistent private inode serializes Supernet, including recovery of
     * its symlink after a crash. Never remove an unrecognized bootstrap lock. */
    if (symlink(OWNER_LOCK, USB_LOCK) && !(errno == EEXIST && own_lock())) {
        sn_log(LOG_ERR, "USB controller is busy or its shared lock is unavailable");
        close(fd);
        return -1;
    }
    return fd;
}

int sn_kobo_unlock(int fd)
{
    int rc = 0;
    if (!own_lock() || unlink(USB_LOCK)) {
        sn_log(LOG_ERR, "could not release the Supernet USB lock");
        rc = -1;
    }
    close(fd);
    return rc;
}

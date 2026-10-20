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

int sn_copy(const char *source, const char *destination, mode_t mode)
{
    int input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (input < 0) {
        sn_log(LOG_ERR, "read %s: %s", source, strerror(errno));
        return -1;
    }
    if (fstat(input, &st) || !S_ISREG(st.st_mode)) {
        close(input);
        return -1;
    }
    char temporary[512];
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", destination) >=
        (int)sizeof(temporary)) {
        close(input);
        return -1;
    }
    int output = mkstemp(temporary), rc = -1;
    if (output < 0) {
        sn_log(LOG_ERR, "create copy of %s at %s: %s", source, destination, strerror(errno));
        close(input);
        return -1;
    }
    /* FAT may reject chmod; its mount determines the visible mode. On other
     * filesystems preserve executable modes. Private runtime copies are strict. */
    if (fchmod(output, mode) &&
        ((strncmp(destination, "/mnt/us/", 8) && strncmp(destination, "/mnt/onboard/", 13)) ||
         errno != EPERM)) {
        goto done;
    }
    char buffer[16384];
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            goto done;
        }
        if (count == 0) {
            break;
        }
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(output, buffer + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                goto done;
            }
            offset += written;
        }
    }
    if (fsync(output) || rename(temporary, destination)) {
        goto done;
    }
    rc = 0;
done:
    if (rc) {
        sn_log(LOG_ERR, "copy %s to %s: %s", source, destination, strerror(errno));
    }
    close(input);
    close(output);
    unlink(temporary);
    return rc;
}

int sn_mkdir(const char *path, mode_t mode)
{
    struct stat st;
    if (mkdir(path, mode) && errno != EEXIST) {
        sn_log(LOG_ERR, "create directory %s: %s", path, strerror(errno));
        return -1;
    }
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) {
        sn_log(LOG_ERR, "unsafe directory: %s", path);
        return -1;
    }
    if (chmod(path, mode)) {
        sn_log(LOG_ERR, "set permissions on %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int sn_read_text(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    ssize_t count = read(fd, out, size - 1);
    close(fd);
    if (count < 0) {
        return -1;
    }
    out[count] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

int sn_write_text(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    size_t size = strlen(value);
    int rc = write(fd, value, size) == (ssize_t)size ? 0 : -1;
    if (close(fd)) {
        rc = -1;
    }
    return rc;
}

bool sn_is_mounted(const char *path)
{
    FILE *file = fopen("/proc/mounts", "r");
    if (!file) {
        return false;
    }
    char line[1024], mountpoint[512];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%*s %511s", mountpoint) == 1 && !strcmp(path, mountpoint)) {
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

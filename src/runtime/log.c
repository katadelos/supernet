#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static bool quiet;
static char first_error[1024], child_diagnostic[1024];
bool sn_quiet(bool value)
{
    bool previous = quiet;
    quiet = value;
    return previous;
}

void sn_error_clear(void)
{
    first_error[0] = child_diagnostic[0] = 0;
}

const char *sn_error(void)
{
    return *first_error ? first_error : child_diagnostic;
}

void sn_log_append(const char *path, const char *data, size_t length)
{
    int saved_errno = errno;
    /* Logging must not block a control request or keep userstore files open. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }
    struct stat st;
    if (flock(fd, LOCK_EX | LOCK_NB) || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid ||
        (st.st_mode & 0077) || st.st_nlink != 1) {
        goto done;
    }
    if (length > SN_LOG_LIMIT) {
        data += length - SN_LOG_LIMIT;
        length = SN_LOG_LIMIT;
    }
    if (st.st_size > (off_t)(SN_LOG_LIMIT - length) && ftruncate(fd, 0)) {
        goto done;
    }
    while (length) {
        ssize_t count = write(fd, data, length);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        data += count;
        length -= (size_t)count;
    }
done:
    close(fd);
    errno = saved_errno;
}

static void record_log(int priority, const char *message)
{
    if (quiet) {
        return;
    }
    if (priority <= LOG_ERR && !*first_error) {
        snprintf(first_error, sizeof(first_error), "%s", message);
    }
    int saved_errno = errno;
    syslog(priority, "%s", message);
    fprintf(stderr, "supernet: %s\n", message);
    char timestamp[32] = "unknown-time";
    time_t now = time(NULL);
    struct tm tm;
    if (gmtime_r(&now, &tm)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    const char *level = "INFO";
    if (priority <= LOG_ERR) {
        level = "ERROR";
    } else if (priority == LOG_WARNING) {
        level = "WARN";
    }
    char record[1200];
    snprintf(record, sizeof(record), "%s [%ld] %s %s\n", timestamp, (long)getpid(), level, message);
    sn_log_append(SN_LOG, record, strlen(record));
    errno = saved_errno;
}

void sn_log(int priority, const char *fmt, ...)
{
    int saved_errno = errno;
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    record_log(priority, message);
    errno = saved_errno;
}

int sn_log_pipe(int pipefd[2])
{
    if (pipe2(pipefd, O_CLOEXEC)) {
        sn_log(LOG_ERR, "create diagnostic pipe: %s", strerror(errno));
        return -1;
    }
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK)) {
        sn_log(LOG_ERR, "configure diagnostic pipe: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    return 0;
}

void sn_log_drain(int fd, const char *program)
{
    if (fd < 0) {
        return;
    }
    int saved_errno = errno;
    char buffer[768], message[1024];
    /* Bound each drain so noisy children cannot starve service requests. */
    for (unsigned i = 0; i < 16; i++) {
        ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        buffer[count] = 0;
        char *save;
        for (char *line = strtok_r(buffer, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            snprintf(message, sizeof(message), "%s: %s", program, line);
            if (!quiet) {
                snprintf(child_diagnostic, sizeof(child_diagnostic), "%s", message);
            }
            record_log(LOG_INFO, message);
        }
    }
    errno = saved_errno;
}

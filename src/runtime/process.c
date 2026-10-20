#include "runtime/runtime.h"
#include "supernet.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

uint64_t sn_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
}

void sn_sleep(unsigned ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

int sn_run(char *const argv[], unsigned timeout_ms)
{
    int pipefd[2];
    if (sn_log_pipe(pipefd)) {
        return -1;
    }
    pid_t pid = fork();
    int status;
    if (pid < 0) {
        sn_log(LOG_ERR, "fork %s: %s", argv[0], strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (!pid) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        setpgid(0, 0);
        execv(argv[0], argv);
        dprintf(STDERR_FILENO, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    setpgid(pid, pid);
    uint64_t until = sn_now() + timeout_ms;
    for (;;) {
        sn_log_drain(pipefd[0], argv[0]);
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            sn_log_drain(pipefd[0], argv[0]);
            close(pipefd[0]);
            if (WIFEXITED(status)) {
                int rc = WEXITSTATUS(status);
                if (rc) {
                    sn_log(LOG_ERR, "%s exited with status %d", argv[0], rc);
                }
                return rc;
            }
            sn_log(LOG_ERR, "%s terminated by signal %d", argv[0], WTERMSIG(status));
            return -1;
        }
        if (result < 0 && errno != EINTR) {
            sn_log(LOG_ERR, "wait for %s: %s", argv[0], strerror(errno));
            close(pipefd[0]);
            return -1;
        }
        if (sn_now() >= until) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            sn_log_drain(pipefd[0], argv[0]);
            close(pipefd[0]);
            sn_log(LOG_ERR, "%s timed out", argv[0]);
            return -1;
        }
        sn_sleep(25);
    }
}

/* Calculate from one clock sample: reading it twice can underflow at expiry. */
int sn_deadline_remaining(uint64_t deadline)
{
    uint64_t now = sn_now();
    if (now >= deadline) {
        return 0;
    }
    uint64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

/* Return 1 for a live process, 0 for an exited process, and -1 on read errors. */
static int process_parent(pid_t pid, pid_t *parent)
{
    char path[64], line[1024], state;
    long ppid;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    }
    /* comm may contain newlines, so read the record instead of its first line. */
    ssize_t length;
    do {
        length = read(fd, line, sizeof(line) - 1);
    } while (length < 0 && errno == EINTR);
    int read_error = errno;
    close(fd);
    if (length < 0) {
        return read_error == ENOENT || read_error == ESRCH ? 0 : -1;
    }
    if (!length) {
        return 0;
    }
    line[length] = 0;
    char *tail = strrchr(line, ')');
    if (!tail || sscanf(tail + 1, " %c %ld", &state, &ppid) != 2) {
        return -1;
    }
    *parent = (pid_t)ppid;
    return state != 'Z' && state != 'X';
}

static int executable_matches(pid_t pid, const char *expected)
{
    char path[64], actual[512];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t length = readlink(path, actual, sizeof(actual) - 1);
    if (length < 0) {
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    }
    actual[length] = 0;
    size_t expected_length = strlen(expected);
    if (strncmp(actual, expected, expected_length)) {
        return 0;
    }
    const char *suffix = actual + expected_length;
    return !*suffix || !strcmp(suffix, " (deleted)");
}

int sn_processes(const char *executable, pid_t parent, int signal_number)
{
    DIR *dir = opendir("/proc");
    if (!dir) {
        sn_log(LOG_ERR, "open process list: %s", strerror(errno));
        return -1;
    }
    int count = 0;
    bool failed = false;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) {
                failed = true;
            }
            break;
        }
        char *end;
        long number = strtol(entry->d_name, &end, 10);
        if (*end || number <= 0 || number > INT_MAX) {
            continue;
        }
        pid_t pid = (pid_t)number, ppid;
        int alive = process_parent(pid, &ppid);
        if (alive < 0) {
            failed = true;
        }
        if (alive <= 0 || (parent && parent != ppid)) {
            continue;
        }
        if (executable) {
            int matches = executable_matches(pid, executable);
            if (matches < 0) {
                failed = true;
            }
            if (matches <= 0) {
                continue;
            }
        }
        count++;
        if (signal_number && kill(pid, signal_number) && errno != ESRCH) {
            failed = true;
        }
    }
    closedir(dir);
    if (failed) {
        sn_log(LOG_ERR, "could not inspect or signal all matching processes");
        return -1;
    }
    return count;
}

static int wait_processes(const char *executable, pid_t parent, unsigned timeout_ms)
{
    uint64_t deadline = sn_now() + timeout_ms;
    for (;;) {
        int count = sn_processes(executable, parent, 0);
        if (count <= 0) {
            return count;
        }
        if (!sn_deadline_remaining(deadline)) {
            return count;
        }
        sn_sleep(25);
    }
}

int sn_processes_stop(const char *executable, pid_t parent)
{
    if (sn_processes(executable, parent, SIGTERM) < 0) {
        return -1;
    }
    int remaining = wait_processes(executable, parent, 3000);
    if (remaining <= 0) {
        return remaining;
    }
    if (sn_processes(executable, parent, SIGKILL) < 0) {
        return -1;
    }
    remaining = wait_processes(executable, parent, 1000);
    if (remaining != 0) {
        sn_log(LOG_ERR, "processes remain after forced shutdown");
        return -1;
    }
    return 0;
}

int sn_process_wait(pid_t pid, unsigned timeout_ms)
{
    uint64_t deadline = sn_now() + timeout_ms;
    for (;;) {
        pid_t parent;
        int alive = process_parent(pid, &parent);
        if (alive <= 0) {
            return alive;
        }
        if (!sn_deadline_remaining(deadline)) {
            return -1;
        }
        sn_sleep(25);
    }
}

#include "access/access.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define ADBD SN_SOURCE_BIN "adbd"
#define ADBD_RUNTIME SN_BIN "adbd"
static pid_t adb_pid;
static int adb_log = -1;

int sn_adb_log_fd(void)
{
    return adb_log;
}

static void drain_adb_log(void)
{
    if (adb_log < 0) {
        return;
    }
    char buffer[4096];
    /* Keep requests responsive even when adbd produces continuous output. */
    for (unsigned i = 0; i < 16; i++) {
        ssize_t count = read(adb_log, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (count <= 0) {
            close(adb_log);
            adb_log = -1;
            break;
        }
        sn_log_append(SN_RUN "/adbd.log", buffer, (size_t)count);
    }
}

void sn_adb_reap(void)
{
    drain_adb_log();
    if (adb_pid <= 0) {
        return;
    }
    pid_t result = waitpid(adb_pid, NULL, WNOHANG);
    if (result == adb_pid || (result < 0 && errno == ECHILD)) {
        adb_pid = 0;
        drain_adb_log();
        if (adb_log >= 0) {
            close(adb_log);
            adb_log = -1;
        }
    }
}

static int adb_processes(void)
{
    sn_adb_reap();
    return sn_processes(ADBD_RUNTIME, 0, 0);
}

bool sn_adb_running(void)
{
    return adb_processes() > 0;
}

static const char *adb_problem(void)
{
    if (access(ADBD, X_OK)) {
        return "ADB is not installed.";
    }
    if (access("/dev/android_adb", R_OK | W_OK) && access("/dev/usb-ffs/adb/ep0", R_OK | W_OK)) {
        return "USB is not configured for ADB. Select ADB in the device USB configuration first.";
    }
    struct stat keys;
    if (stat(SN_SDK_ROOT "/etc/adb/adb_keys", &keys) || !keys.st_size) {
        return "Add your computer's ADB public key to the adb_keys file first.";
    }
    return "";
}

static void run_adb(pid_t parent, int lock, int output)
{
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) {
        _exit(127);
    }
    if (setsid() < 0) {
        _exit(127);
    }
    close(lock);
    int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0 ||
        dup2(input, STDIN_FILENO) < 0) {
        _exit(127);
    }
    if (output > STDERR_FILENO) {
        close(output);
    }
    if (input > STDERR_FILENO) {
        close(input);
    }
    unsetenv("ADBD_TCP_PORT");
    execl(ADBD_RUNTIME, "adbd", (char *)NULL);
    _exit(127);
}

static int start_adb(int lock)
{
    const char *error = adb_problem();
    if (*error) {
        sn_log(LOG_ERR, "%s", error);
        return -1;
    }
    if (sn_mkdir(SN_RUN "/bin", 0700) || sn_copy(ADBD, ADBD_RUNTIME, 0700)) {
        return -1;
    }
    int pipefd[2];
    if (sn_log_pipe(pipefd)) {
        return -1;
    }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) {
        sn_log(LOG_ERR, "fork ADB: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (!child) {
        close(pipefd[0]);
        run_adb(parent, lock, pipefd[1]);
    }
    close(pipefd[1]);
    adb_log = pipefd[0];
    adb_pid = child;
    sn_sleep(500);
    if (adb_processes() > 0) {
        return 0;
    }
    if (adb_pid > 0) {
        kill(adb_pid, SIGKILL);
        while (waitpid(adb_pid, NULL, 0) < 0 && errno == EINTR) {
        }
        sn_adb_reap();
    }
    sn_log(LOG_ERR, "ADB failed to start; see " SN_RUN "/adbd.log.");
    return -1;
}

int sn_adb_set(bool enabled)
{
    if (enabled && sn_package_busy()) {
        sn_log(LOG_ERR, "A package operation is running.");
        return -1;
    }
    if (sn_mkdir(SN_RUN, 0700)) {
        return -1;
    }
    int lock = open(SN_RUN "/adb.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) {
        return -1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        close(lock);
        return -1;
    }
    int rc = 0;
    int running = adb_processes();
    if (running < 0) {
        rc = -1;
    } else if (enabled && !running) {
        rc = start_adb(lock);
    } else if (!enabled && running) {
        rc = sn_processes_stop(ADBD_RUNTIME, 0);
        sn_adb_reap();
        if (rc) {
            sn_log(LOG_ERR, "Could not stop Supernet ADB; see " SN_RUN "/adbd.log.");
        }
    }
    close(lock);
    return rc;
}

struct sn_adb_status sn_adb_status(bool usb_ssh)
{
    bool running = sn_adb_running();
    struct sn_adb_status status = {
        .available = !access(ADBD, X_OK) || running,
        .running = running,
        .error = "",
    };
    if (!running) {
        status.error = usb_ssh ? "Stop USB SSH before starting ADB." : adb_problem();
    }
    return status;
}

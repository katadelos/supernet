#include "platform/lipc.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <openlipc.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

/* Firmware supplies liblipc. Keep its ABI in openlipc.h, not duplicate declarations.
 * Loading the firmware's unversioned entry point avoids linking/distributing its
 * proprietary library and handles the different SONAMEs of the three ABIs. */
enum operation { GET, SET, EVENT };
struct result {
    int code, value;
};

static struct result invoke(enum operation op, int value, const char *name)
{
    struct result result = {.code = LIPC_ERROR_INTERNAL, .value = value};
    void *lib = dlopen("/usr/lib/liblipc.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        sn_log(LOG_ERR, "load firmware liblipc: %s", dlerror());
        return result;
    }
#define LOAD(fn)                                                                                   \
    __typeof__(&fn) p_##fn = (__typeof__(&fn))dlsym(lib, #fn);                                     \
    if (!p_##fn) {                                                                                 \
        sn_log(LOG_ERR, "liblipc missing %s", #fn);                                                \
        goto done;                                                                                 \
    }
    LOAD(LipcOpenEx);
    LOAD(LipcOpenNoName);
    LOAD(LipcClose);
    LOAD(LipcGetIntProperty);
    LOAD(LipcSetIntProperty);
    LOAD(LipcNewEvent);
    LOAD(LipcSendEvent);
    LOAD(LipcEventFree);
    LIPCcode code = LIPC_OK;
    /* Matches firmware lipc-send-event: OpenEx(source), NewEvent, SendEvent. */
    LIPC *lipc = op == EVENT ? p_LipcOpenEx("com.lab126.hal", &code) : p_LipcOpenNoName();
    if (!lipc) {
        result.code = code ? code : LIPC_ERROR_INIT_DBUS;
        goto done;
    }
    if (op == GET) {
        result.code =
            p_LipcGetIntProperty(lipc, "com.lab126.volumd", "useUsbForNetwork", &result.value);
    } else if (op == SET) {
        result.code = p_LipcSetIntProperty(lipc, "com.lab126.volumd", "useUsbForNetwork", value);
    } else {
        LIPCevent *event = p_LipcNewEvent(lipc, name);
        if (event) {
            result.code = p_LipcSendEvent(lipc, event);
            p_LipcEventFree(event);
        }
    }
    p_LipcClose(lipc);
done:
    dlclose(lib);
    return result;
}

/* Some firmware LIPC calls block for 10 seconds or longer. An isolated worker
 * bounds the complete operation, including dlopen/connect, without signals
 * interrupting library internals in the service. No persistent child or shell. */
static int call(enum operation op, int *value, const char *name)
{
    int pipefd[2], status;
    struct result result;
    if (pipe2(pipefd, O_CLOEXEC)) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (!pid) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        close(pipefd[0]);
        result = invoke(op, *value, name);
        ssize_t written = write(pipefd[1], &result, sizeof(result));
        _exit(written == sizeof(result) ? 0 : 1);
    }
    close(pipefd[1]);
    struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
    int ready;
    uint64_t end = sn_now() + 2500;
    do {
        int remaining = sn_deadline_remaining(end);
        ready = poll(&pfd, 1, remaining);
    } while (ready < 0 && errno == EINTR && sn_deadline_remaining(end));
    int rc = -1;
    if (ready > 0 && read(pipefd[0], &result, sizeof(result)) == sizeof(result)) {
        if (result.code == LIPC_OK) {
            *value = result.value;
            rc = 0;
        } else {
            sn_log(LOG_WARNING, "LIPC %s failed: code %d", name, result.code);
        }
    } else {
        sn_log(LOG_ERR, "LIPC %s timed out or worker failed", name);
    }
    close(pipefd[0]);
    /* The worker has already written its result; killing it also bounds exit. */
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return rc;
}

int sn_lipc_get(int *value)
{
    *value = 0;
    return call(GET, value, "useUsbForNetwork");
}

int sn_lipc_set(int value)
{
    return call(SET, &value, "useUsbForNetwork");
}

int sn_lipc_event(const char *name)
{
    int value = 0;
    return call(EVENT, &value, name);
}

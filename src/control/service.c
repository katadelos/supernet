#include "access/access.h"
#include "control/control.h"
#include "network/network.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

struct service {
    pid_t ssh_pid;
    int ssh_log;
    struct config active;
    bool firewall;
    char wifi_address[16];
    char wifi_interface[IFNAMSIZ];
    unsigned active_sessions;
    unsigned usb_failures;
    bool usb_failed;
};

/* Only the signal handler shares this flag; normal state belongs to sn_serve. */
static volatile sig_atomic_t exiting;
static int stop_orphan_sessions(void);

static void terminate(int signal_number)
{
    (void)signal_number;
    exiting = 1;
}

static int wifi_rule(struct service *service, const struct config *cfg, bool add)
{
    return sn_firewall_set(cfg, service->wifi_interface, service->wifi_address, add);
}

static void reap_ssh(struct service *service)
{
    sn_log_drain(service->ssh_log, "dropbear");
    int status;
    if (service->ssh_pid > 0 && waitpid(service->ssh_pid, &status, WNOHANG) == service->ssh_pid) {
        sn_log_drain(service->ssh_log, "dropbear");
        close(service->ssh_log);
        service->ssh_log = -1;
        if (WIFEXITED(status)) {
            sn_log(LOG_WARNING,
                   "Dropbear exited with status %d; USB mode retained for recovery",
                   WEXITSTATUS(status));
        } else {
            sn_log(LOG_WARNING,
                   "Dropbear terminated by signal %d; USB mode retained for recovery",
                   WTERMSIG(status));
        }
        service->ssh_pid = 0;
        stop_orphan_sessions();
    }
}

/* Dropbear puts each connection in a separate session, so signalling only its
 * listener leaves established Wi-Fi connections alive. Identify its direct
 * children while the listener is stopped; never signal another SSH server. */
static int signal_sessions(pid_t parent, int signal_number)
{
    const char *executable = parent == getpid() ? SN_BIN "dropbearmulti" : NULL;
    return sn_processes(executable, parent, signal_number);
}

static int stop_orphan_sessions(void)
{
    /* Connection processes can be reparented to init when the listener exits.
     * The private executable identifies our SSH processes on every supported
     * kernel, including those without PR_SET_CHILD_SUBREAPER. */
    int rc = sn_processes_stop(SN_BIN "dropbearmulti", 0);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    return rc;
}

static int stop_ssh(struct service *service)
{
    if (service->ssh_pid > 0) {
        pid_t pid = service->ssh_pid;
        if (!kill(pid, SIGSTOP)) {
            int status;
            pid_t result;
            do {
                result = waitpid(pid, &status, WUNTRACED);
            } while (result < 0 && errno == EINTR);
            if (result == pid && WIFSTOPPED(status)) {
                signal_sessions(pid, SIGTERM);
                uint64_t until = sn_now() + 3000;
                while (signal_sessions(pid, 0) > 0 && sn_now() < until) {
                    sn_sleep(25);
                }
                signal_sessions(pid, SIGKILL);
                kill(pid, SIGCONT);
            } else if (result == pid || (result < 0 && errno == ECHILD)) {
                service->ssh_pid = 0;
            }
        }
        if (service->ssh_pid) {
            kill(pid, SIGTERM);
        }
        uint64_t until = sn_now() + 3000;
        while (service->ssh_pid && sn_now() < until) {
            pid_t rc = waitpid(pid, NULL, WNOHANG);
            if (rc == pid || (rc < 0 && errno == ECHILD)) {
                service->ssh_pid = 0;
                break;
            }
            sn_sleep(25);
        }
        if (service->ssh_pid) {
            kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            }
            service->ssh_pid = 0;
        }
    }
    int cleanup = stop_orphan_sessions();
    sn_log_drain(service->ssh_log, "dropbear");
    if (service->ssh_log >= 0) {
        close(service->ssh_log);
        service->ssh_log = -1;
    }
    if (service->firewall || sn_firewall_pending()) {
        if (wifi_rule(service, &service->active, false)) {
            sn_log(LOG_ERR, "could not remove Supernet Wi-Fi firewall rule");
            return -1;
        }
        service->firewall = false;
    }
    return cleanup;
}

static int start_ssh(struct service *service, const struct config *cfg)
{
    sn_log(LOG_INFO,
           "starting Dropbear on %s:%u, interface %s, wifi=%s",
           cfg->address,
           cfg->port,
           cfg->interface,
           cfg->wifi ? "true" : "false");
    char listen_usb[32], listen_wifi[32];
    snprintf(listen_usb, sizeof(listen_usb), "%s:%u", cfg->address, cfg->port);
    snprintf(listen_wifi, sizeof(listen_wifi), "%s:%u", service->wifi_address, cfg->port);
    char *args[24] = {SN_BIN "dropbear",
                      "-F",
                      "-E",
                      "-s",
                      "-r",
                      SN_HOSTKEY,
                      "-D",
                      SN_RUN,
                      "-P",
                      SN_RUN "/dropbear.pid",
                      "-K",
                      "15"};
    unsigned index = 12;
    if (cfg->usb) {
        args[index++] = "-p";
        args[index++] = listen_usb;
    }
    if (cfg->wifi) {
        args[index++] = "-p";
        args[index++] = listen_wifi;
    }
    if (!(cfg->usb && cfg->wifi)) {
        args[index++] = "-l";
        args[index++] = cfg->usb ? (char *)cfg->interface : service->wifi_interface;
    }
    if (unlink(SN_RUN "/dropbear.pid") && errno != ENOENT) {
        sn_log(LOG_ERR, "remove stale Dropbear PID file: %s", strerror(errno));
        return -1;
    }
    int pipefd[2];
    if (sn_log_pipe(pipefd)) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        sn_log(LOG_ERR, "fork Dropbear: %s", strerror(errno));
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
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() == 1) {
            _exit(127);
        }
        execv(args[0], args);
        dprintf(STDERR_FILENO, "exec %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    service->ssh_pid = pid;
    close(pipefd[1]);
    service->ssh_log = pipefd[0];
    /* Dropbear writes its PID file after opening the listeners. A local TCP
     * probe cannot reach a USB-only listener bound with SO_BINDTODEVICE. */
    uint64_t until = sn_now() + 3000;
    while (sn_now() < until) {
        reap_ssh(service);
        if (!service->ssh_pid) {
            return -1;
        }
        FILE *file = fopen(SN_RUN "/dropbear.pid", "r");
        if (file) {
            long ready_pid = 0;
            int count = fscanf(file, "%ld", &ready_pid);
            fclose(file);
            if (count == 1 && ready_pid == service->ssh_pid) {
                sn_sleep(100);
                reap_ssh(service);
                return service->ssh_pid > 0 ? 0 : -1;
            }
        }
        sn_sleep(50);
    }
    sn_log(LOG_ERR, "Dropbear did not become ready within 3 seconds");
    stop_ssh(service);
    return -1;
}

static int start_network(struct service *service, bool usb, bool wifi)
{
    reap_ssh(service);
    if (service->ssh_pid > 0) {
        if (service->active.usb != usb || service->active.wifi != wifi) {
            sn_log(LOG_ERR, "Stop access before choosing another connection");
            return -1;
        }
        return 0;
    }
    struct config cfg;
    if (sn_config(SN_CONFIG, &cfg)) {
        return -1;
    }
    cfg.usb = usb;
    cfg.wifi = wifi;
    if (sn_access_check(&cfg, service->ssh_pid > 0)) {
        return -1;
    }
    if (sn_copy(SN_SDK_ROOT "/usr/sbin/dropbear", SN_BIN "dropbearmulti", 0700)) {
        return -1;
    }
    if (sn_keys_prepare()) {
        return -1;
    }
    if (cfg.wifi && sn_wifi_address(service->wifi_interface, service->wifi_address)) {
        sn_log(LOG_ERR, "Wi-Fi access requires an active wireless IPv4 address");
        return -1;
    }
    service->active = cfg;
    if (cfg.usb && sn_usb_start(&cfg)) {
        goto fail;
    }
    if (cfg.wifi) {
        if (wifi_rule(service, &cfg, true)) {
            goto fail;
        }
        service->firewall = true;
    }
    if (start_ssh(service, &cfg)) {
        goto fail;
    }
    service->usb_failed = false;
    service->usb_failures = 0;
    sn_log(LOG_INFO, "SSH ready at %s:%u (%s)", cfg.address, cfg.port, SN_PLATFORM);
    return 0;
fail:
    /* Retain the triggering error or child diagnostic before cleanup logs. */
    sn_log(LOG_ERR, "%s", *sn_error() ? sn_error() : "Could not start SSH access.");
    stop_ssh(service);
    return -1;
}

static int stop_network(struct service *service)
{
    if (stop_ssh(service)) {
        return -1;
    }
    service->active_sessions = 0;
    sn_wake_update(sn_awake_requested());
    if (sn_usb_recover()) {
        return -1;
    }
    service->usb_failed = false;
    service->usb_failures = 0;
    sn_log(LOG_INFO, "Supernet SSH stopped; userstore remains mounted");
    return 0;
}

static int dispatch(struct service *service, const char *command, char *reply, size_t size)
{
    int rc = 0;
    reap_ssh(service);
    if (!strcmp(command, "status-json")) {
        int owned = sn_ssh_processes(0);
        struct sn_service_view view = {
            .service_running = true,
            .ssh_running = owned > 0,
            .listener_running = service->ssh_pid > 0,
            .usb_failed = service->usb_failed,
            .config = service->active,
            .sessions = service->active_sessions,
        };
        snprintf(view.wifi_address, sizeof(view.wifi_address), "%s", service->wifi_address);
        if (owned >= 0 && !sn_status(reply, size, &view)) {
            return 0;
        }
        snprintf(reply, size, "ERROR could not read Supernet status\n");
        return -1;
    }
    sn_error_clear();
    if ((!strncmp(command, "start-", 6) || !strcmp(command, "stop")) && sn_package_busy()) {
        snprintf(reply, size, "ERROR package operation in progress\n");
        return -1;
    }
    if (!strcmp(command, "adb-start") && service->ssh_pid > 0 && service->active.usb) {
        snprintf(reply, size, "ERROR Stop USB SSH before starting ADB\n");
        return -1;
    }
    if (!strcmp(command, "adb-start")) {
        rc = sn_adb_set(true);
    } else if (!strcmp(command, "adb-stop")) {
        rc = sn_adb_set(false);
    } else if (!strcmp(command, "awake-on")) {
        rc = sn_awake_set(true);
    } else if (!strcmp(command, "awake-off")) {
        rc = sn_awake_set(false);
    } else if (!strcmp(command, "start-usb")) {
        rc = start_network(service, true, false);
    } else if (!strcmp(command, "start-wifi")) {
        rc = start_network(service, false, true);
    } else if (!strcmp(command, "start-both")) {
        rc = start_network(service, true, true);
    } else if (!strcmp(command, "stop")) {
        rc = stop_network(service);
    } else if (!strcmp(command, "prepare-remove")) {
        if (service->ssh_pid > 0 || sn_ssh_processes(0) != 0) {
            snprintf(reply,
                     size,
                     "ERROR Stop Supernet SSH before upgrading or removing it; active remote "
                     "sessions are preserved\n");
            return -1;
        }
        rc = sn_adb_set(false);
        if (!rc) {
            rc = stop_network(service);
        }
        if (!rc) {
            rc = sn_ssh_recover();
        }
        if (!rc) {
            rc = sn_usb_recover();
        }
        if (!rc) {
            exiting = 1;
        }
    } else if (!strcmp(command, "shutdown")) {
        rc = sn_adb_set(false);
        if (stop_network(service)) {
            rc = -1;
        }
        if (!rc) {
            exiting = 1;
        }
    } else {
        rc = -1;
    }
    const char *detail = sn_error();
    snprintf(reply,
             size,
             "%s %s%s%s\n",
             rc ? "ERROR" : "OK",
             command,
             rc ? ": " : "",
             rc ? (*detail ? detail : "Could not change remote access; see " SN_LOG) : "");
    return rc;
}

int sn_serve(bool background)
{
    struct service state = {.ssh_log = -1};
    struct service *service = &state;
    if (sn_mkdir(SN_RUN, 0700) || sn_config(SN_CONFIG, &service->active)) {
        return 1;
    }
    int lock = open(SN_RUN "/lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        sn_log(LOG_ERR, "service already running or cannot lock runtime directory");
        if (lock >= 0) {
            close(lock);
        }
        return 1;
    }
    if (background && daemon(0, 0)) {
        sn_log(LOG_ERR, "daemonize control service: %s", strerror(errno));
        close(lock);
        return 1;
    }
    struct sigaction action = {.sa_handler = terminate};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, SN_SOCKET);
    unlink(SN_SOCKET);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        chmod(SN_SOCKET, 0600) || listen(listener, 4)) {
        sn_log(LOG_ERR, "control socket: %s", strerror(errno));
        if (listener >= 0) {
            close(listener);
        }
        close(lock);
        return 1;
    }
    sn_log(LOG_INFO, "Supernet %s control service ready (%s)", SN_VERSION, SN_PLATFORM);
    int rc = 0;
    uint64_t next_update = 0;
    while (!exiting) {
        sn_adb_reap();
        /* Log traffic must not accelerate USB retries or wake-lease updates. */
        if (sn_now() >= next_update) {
            reap_ssh(service);
            int sessions = service->ssh_pid > 0 ? signal_sessions(service->ssh_pid, 0) : 0;
            service->active_sessions = sessions > 0 ? (unsigned)sessions : 0;
            sn_wake_update(service->active_sessions > 0 || sn_awake_requested());
            if (service->ssh_pid > 0 && service->active.usb) {
                sn_error_clear();
                if (sn_usb_poll(&service->active)) {
                    service->usb_failures++;
                    if (service->usb_failures >= 3) {
                        sn_log(LOG_ERR,
                               "USB configuration failed after three attempts; stopping SSH");
                        stop_ssh(service);
                        service->active_sessions = 0;
                        service->usb_failed = true;
                    }
                } else {
                    service->usb_failures = 0;
                }
            }
            next_update = sn_now() + 1000;
        }
        struct pollfd pfd[] = {
            {.fd = listener, .events = POLLIN},
            {.fd = sn_adb_log_fd(), .events = POLLIN},
        };
        int ready = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), sn_deadline_remaining(next_update));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            sn_log(LOG_ERR, "poll control socket: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (!ready) {
            continue;
        }
        if (pfd[1].revents) {
            sn_adb_reap();
        }
        if (!pfd[0].revents) {
            continue;
        }
        if (!(pfd[0].revents & POLLIN)) {
            sn_log(LOG_ERR, "control socket became unavailable (events=%x)", pfd[0].revents);
            rc = -1;
            break;
        }
        /* The firmware libc provides accept(), whereas its accept4 symbol
         * may be an ENOSYS stub. This daemon is single-threaded, so setting
         * close-on-exec before processing a request cannot race with exec. */
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            sn_log(LOG_ERR, "accept control request: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (fcntl(client, F_SETFD, FD_CLOEXEC)) {
            sn_log(LOG_ERR, "protect control descriptor: %s", strerror(errno));
            close(client);
            rc = -1;
            break;
        }
        struct ucred peer;
        socklen_t peer_size = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) || peer.uid != 0) {
            close(client);
            continue;
        }
        struct timeval timeout = {.tv_sec = 1};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        char command[32], reply[SN_REPLY_SIZE];
        size_t used = 0;
        bool complete = false;
        while (used < sizeof(command) - 1) {
            ssize_t count = read(client, command + used, 1);
            if (count != 1) {
                break;
            }
            if (command[used] == '\n') {
                complete = true;
                break;
            }
            if (!command[used]) {
                break;
            }
            used++;
        }
        command[used] = 0;
        if (complete) {
            dispatch(service, command, reply, sizeof(reply));
        } else {
            snprintf(reply, sizeof(reply), "ERROR invalid request\n");
        }
        ssize_t ignored = write(client, reply, strlen(reply));
        (void)ignored;
        close(client);
    }
    sn_awake_set(false);
    if (sn_adb_set(false)) {
        rc = -1;
    }
    if (stop_network(service)) {
        rc = -1;
    }
    /* Even when storage restoration is unsafe/unavailable, end our listener. */
    if (stop_ssh(service)) {
        rc = -1;
    }
    close(listener);
    unlink(SN_SOCKET);
    close(lock);
    return rc ? 1 : 0;
}

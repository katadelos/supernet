#include "control/control.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* A missing control socket is an ordinary stopped state. A live socket which
 * fails to reply is an error, never permission to launch a competing daemon. */
int sn_request(const char *command, bool optional, bool display)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 1;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, SN_SOCKET);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        int missing = errno == ENOENT || errno == ECONNREFUSED;
        close(fd);
        return optional && missing ? -1 : 1;
    }
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) || peer.uid != 0) {
        close(fd);
        return 1;
    }
    struct timeval timeout = {.tv_sec = !strcmp(command, "status-json") ? 3 : 75};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char buffer[SN_REPLY_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "%s\n", command);
    if (send(fd, buffer, (size_t)len, MSG_NOSIGNAL) != len) {
        close(fd);
        return 1;
    }
    size_t used = 0;
    while (used < sizeof(buffer) - 1) {
        ssize_t count = read(fd, buffer + used, sizeof(buffer) - 1 - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
        if (memchr(buffer, '\n', used)) {
            break;
        }
    }
    close(fd);
    buffer[used] = 0;
    if (!used || buffer[used - 1] != '\n') {
        fputs("Supernet did not reply; check status before retrying.\n", stderr);
        return 1;
    }
    if (!strncmp(buffer, "OK ", 3) &&
        (!strcmp(command, "prepare-remove") || !strcmp(command, "shutdown"))) {
        if (sn_process_wait(peer.pid, 10000)) {
            fputs("Supernet has not exited; package removal was refused.\n", stderr);
            return 1;
        }
    }

    int rc = *buffer == '{' || !strncmp(buffer, "OK ", 3) ? 0 : 1;
    if (display || rc) {
        fputs(buffer, rc ? stderr : stdout);
    }
    return rc;
}

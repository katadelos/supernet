#ifndef SUPERNET_CONTROL_H
#define SUPERNET_CONTROL_H

#include "supernet.h"

#define SN_REPLY_SIZE 4096

struct sn_service_view {
    bool service_running;
    bool ssh_running;
    bool listener_running;
    bool usb_failed;
    struct config config;
    char wifi_address[16];
    unsigned sessions;
};

/* Request returns 0 on success, 1 on failure, or -1 for an absent optional service. */
int sn_request(const char *command, bool optional, bool display);
int sn_status(char *out, size_t size, const struct sn_service_view *service);
int sn_serve(bool background);

#endif

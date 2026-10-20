#include "access/access.h"
#include "network/network.h"
#include "platform/lipc.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static int switch_to_network(const struct config *cfg);

int sn_usb_poll(const struct config *cfg)
{
    (void)cfg;
    return 0;
}

enum sn_usb_state sn_usb_state(const struct config *cfg, char *detail, size_t size)
{
    if (!sn_is_mounted("/mnt/us")) {
        snprintf(detail, size, "Eject USB storage and wait for the Kindle userstore to mount.");
        return SN_USB_UNAVAILABLE;
    }
    int mode;
    if (sn_lipc_get(&mode) || (mode != 0 && mode != 1)) {
        snprintf(detail, size, "The Kindle USB controller is unavailable.");
        return SN_USB_UNAVAILABLE;
    }
    bool interface_exists = sn_interface(cfg, false);
    if (sn_usb_owned(cfg)) {
        if (mode == 1 && interface_exists) {
            return SN_USB_OWNED;
        }
        /* A prior attempt may have reserved USB before firmware IPC failed.
         * Retain that intent and allow retry when no interface has appeared. */
        if (!interface_exists) {
            return SN_USB_RETRYABLE;
        }
    }
    if (mode == 1 || interface_exists) {
        snprintf(detail,
                 size,
                 "USB networking belongs to another tool. Supernet will not take it over.");
        return SN_USB_FOREIGN;
    }
    if (!access(SN_RUN "/network", F_OK)) {
        snprintf(detail,
                 size,
                 "The configuration changed while USB access was reserved. Restore it or reboot.");
        return SN_USB_UNAVAILABLE;
    }
    return SN_USB_IDLE;
}

int sn_usb_start(const struct config *cfg)
{
    char detail[256] = "";
    enum sn_usb_state state = sn_usb_state(cfg, detail, sizeof(detail));
    if (!sn_usb_can_start(state)) {
        sn_log(LOG_ERR, "%s", detail);
        return -1;
    }
    if (state != SN_USB_OWNED) {
        if (state == SN_USB_IDLE && sn_usb_mark_owned(cfg)) {
            return -1;
        }
        if (switch_to_network(cfg)) {
            return -1;
        }
    }
    return sn_net_configure(cfg);
}

int sn_usb_stop(const struct config *cfg)
{
    /* Never switch to mass storage: that could unmount running applications.
     * A failed transition that left storage mode untouched can release its
     * reservation without changing firmware state. */
    if (!sn_usb_owned(cfg)) {
        return 0;
    }
    int mode;
    if (sn_lipc_get(&mode)) {
        return -1;
    }
    if (mode == 0 && !sn_interface(cfg, false)) {
        if (unlink(SN_RUN "/network") && errno != ENOENT) {
            return -1;
        }
    }
    return 0;
}

bool sn_usb_needs_cleanup(enum sn_usb_state state)
{
    return state == SN_USB_RETRYABLE;
}

const char *sn_usb_stopped_detail(enum sn_usb_state state)
{
    if (state == SN_USB_OWNED) {
        return "SSH is stopped. USB network mode is retained to keep applications available; "
               "reboot to restore file transfers.";
    }
    if (state == SN_USB_RETRYABLE) {
        return "USB preparation did not finish. Start access to retry, or stop access to release "
               "the reservation.";
    }
    return NULL;
}

static int switch_to_network(const struct config *cfg)
{
    uint64_t end = sn_now() + cfg->timeout * 1000;
    int rc;
    do {
        rc = sn_lipc_set(1);
        if (!rc) {
            break;
        }
        sn_sleep(250);
    } while (sn_now() < end);
    if (rc) {
        return -1;
    }
    /* Firmware queues gadget changes until HAL disconnect notifications. Send
     * each notification once per attempt, then observe actual interface state.
     * Do not infer state from g_ether: newer kernels use other gadget drivers. */
    if (sn_lipc_event("usbUnconfigured")) {
        return -1;
    }
    sn_sleep(250);
    if (sn_lipc_event("usbPlugOut")) {
        return -1;
    }
    unsigned settled = 0;
    while (sn_now() < end) {
        bool ready = sn_interface(cfg, true);
        if (ready && ++settled >= 4) {
            return 0;
        }
        if (!ready) {
            settled = 0;
        }
        sn_sleep(250);
    }
    sn_log(LOG_ERR, "volumd did not reach USB network mode within %u seconds", cfg->timeout);
    return -1;
}

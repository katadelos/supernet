#ifndef SUPERNET_USB_H
#define SUPERNET_USB_H

#include "supernet.h"

enum sn_usb_state {
    SN_USB_UNAVAILABLE,
    SN_USB_IDLE,
    SN_USB_OWNED,
    SN_USB_FOREIGN,
    SN_USB_INCOMPLETE,
    SN_USB_RETRYABLE,
};

enum sn_usb_state sn_usb_state(const struct config *cfg, char *detail, size_t size);
bool sn_usb_can_start(enum sn_usb_state state);
bool sn_usb_needs_cleanup(enum sn_usb_state state);
const char *sn_usb_stopped_detail(enum sn_usb_state state);
int sn_usb_start(const struct config *cfg);
int sn_usb_stop(const struct config *cfg);
int sn_usb_recover(void);
int sn_usb_poll(const struct config *cfg);

/* Private ownership journal shared by the device backends. */
bool sn_usb_owned(const struct config *cfg);
int sn_usb_mark_owned(const struct config *cfg);

#endif

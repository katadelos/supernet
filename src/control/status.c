#include "access/access.h"
#include "control/control.h"
#include "network/network.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct json_writer {
    char *next;
    size_t remaining;
    bool first;
    bool failed;
};

static void json_append(struct json_writer *json, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void json_append(struct json_writer *json, const char *format, ...)
{
    if (json->failed) {
        return;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(json->next, json->remaining, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= json->remaining) {
        json->failed = true;
        return;
    }
    json->next += length;
    json->remaining -= (size_t)length;
}

static void json_key(struct json_writer *json, const char *name)
{
    json_append(json, "%s\"%s\":", json->first ? "" : ",", name);
    json->first = false;
}

static void json_text(struct json_writer *json, const char *name, const char *value)
{
    json_key(json, name);
    json_append(json, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\\' || *p == '"') {
            json_append(json, "\\%c", *p);
        } else if (*p < 32) {
            json_append(json, "\\u%04x", *p);
        } else {
            json_append(json, "%c", *p);
        }
    }
    json_append(json, "\"");
}

static void json_bool(struct json_writer *json, const char *name, bool value)
{
    json_key(json, name);
    json_append(json, "%s", value ? "true" : "false");
}

static void json_uint(struct json_writer *json, const char *name, unsigned value)
{
    json_key(json, name);
    json_append(json, "%u", value);
}

int sn_status(char *out, size_t size, const struct sn_service_view *service)
{
    if (!size) {
        return -1;
    }
    bool was_quiet = sn_quiet(true);
    struct config cfg;
    bool configured = sn_config(SN_CONFIG, &cfg) == 0;
    if (service->listener_running) {
        cfg = service->config;
    }

    bool usb_enabled = service->listener_running && cfg.usb;
    bool wifi_enabled = service->listener_running && cfg.wifi;
    struct sn_adb_status adb = sn_adb_status(usb_enabled);
    char usb_error[256] = "", wifi_error[256] = "";
    enum sn_usb_state usb = sn_usb_state(&cfg, usb_error, sizeof(usb_error));
    if (sn_usb_can_start(usb) && strcmp(cfg.interface, "usb0")) {
        snprintf(usb_error, sizeof(usb_error), "Only the usb0 interface is supported.");
    }
    if (adb.running) {
        snprintf(usb_error, sizeof(usb_error), "Stop ADB before starting USB SSH.");
    }

    char interface[IFNAMSIZ], current_wifi[16] = "";
    bool wifi_available = sn_wifi_address(interface, current_wifi) == 0;
    if (!wifi_available) {
        snprintf(wifi_error, sizeof(wifi_error), "Connect Wi-Fi in the stock reader first.");
    }

    const char *error = sn_access_problem(&cfg, service->ssh_running);
    if (!configured) {
        error = "The Supernet configuration is missing or invalid.";
    }
    if (service->usb_failed) {
        error = "USB configuration failed after repeated attempts. Stop access and start again.";
    }
    bool busy = sn_package_busy();
    bool can_usb = !*error && !*usb_error && sn_usb_can_start(usb) && !service->ssh_running;
    bool can_wifi = !*error && wifi_available && !service->ssh_running;
    bool can_stop = !busy && (service->ssh_running || sn_firewall_pending() ||
                              sn_usb_needs_cleanup(usb) || service->usb_failed);

    char detail[256];
    const char *stopped_detail = sn_usb_stopped_detail(usb);
    if (service->usb_failed) {
        snprintf(detail, sizeof(detail), "%s", error);
    } else if (service->ssh_running && !service->listener_running) {
        snprintf(
            detail,
            sizeof(detail),
            "SSH processes remain without an active listener. Stop access before starting again.");
    } else if (wifi_enabled && (!wifi_available || strcmp(current_wifi, service->wifi_address))) {
        snprintf(detail,
                 sizeof(detail),
                 "Wi-Fi changed. SSH is still bound to its previous address; stop access and start "
                 "again to reconnect.");
    } else if (service->ssh_running) {
        const char *transports = "Wi-Fi";
        if (usb_enabled && wifi_enabled) {
            transports = "USB and Wi-Fi";
        } else if (usb_enabled) {
            transports = "USB";
        }
        snprintf(detail,
                 sizeof(detail),
                 "SSH access is running over %s. Public-key login only.",
                 transports);
    } else if (stopped_detail) {
        snprintf(detail, sizeof(detail), "%s", stopped_detail);
    } else {
        snprintf(detail,
                 sizeof(detail),
                 "Remote access is stopped. Choose a connection when you want to connect from your "
                 "computer.");
    }

    char hostname[256] = "", foreign_usb[16];
    if (gethostname(hostname, sizeof(hostname) - 1)) {
        hostname[0] = 0;
    }
    bool network = sn_interface(&cfg, true) || !sn_address("rndis0", foreign_usb);
    bool adb_endpoint = !access("/dev/android_adb", F_OK) || !access("/dev/usb-ffs/adb/ep0", F_OK);
    bool owned = usb == SN_USB_OWNED || usb == SN_USB_INCOMPLETE || usb == SN_USB_RETRYABLE;
    const char *usb_mode = "other";
    if (adb_endpoint) {
        usb_mode = "adb";
    } else if (network) {
        usb_mode = "network";
    }

    struct json_writer json = {.next = out, .remaining = size, .first = true};
    json_append(&json, "{");
    json_uint(&json, "schema", 1);
    json_text(&json, "version", SN_VERSION);
    json_text(&json, "platform", SN_PLATFORM);
    json_text(&json, "hostname", hostname);
    json_text(&json, "usbMode", usb_mode);
    json_bool(&json, "adbAvailable", adb.available);
    json_bool(&json, "adb", adb.running);
    json_text(&json, "adbError", adb.error);
    json_bool(&json, "keepAwakeAvailable", sn_wake_available());
    json_bool(&json, "keepAwake", sn_awake_requested());
    json_bool(&json, "service", service->service_running);
    json_bool(&json, "network", network);
    json_bool(&json, "ssh", service->ssh_running);
    json_bool(&json, "owned", owned);
    json_bool(&json, "keysReady", sn_keys_ready());
    json_text(&json, "keyPath", SN_KEYS "/authorized_keys");
    json_text(&json, "address", cfg.address);
    json_uint(&json, "prefix", cfg.prefix);
    json_uint(&json, "port", cfg.port);
    json_bool(&json, "canStart", can_usb || can_wifi);
    json_bool(&json, "canStop", can_stop);
    json_uint(&json, "activeSessions", service->sessions);
    json_bool(&json, "keepsAwake", sn_wake_held());
    json_bool(&json, "usbEnabled", usb_enabled);
    json_bool(&json, "wifiEnabled", wifi_enabled);
    json_bool(&json, "wifiAvailable", wifi_available);
    json_text(&json, "wifiAddress", wifi_enabled ? service->wifi_address : current_wifi);
    json_bool(&json, "canStartUsb", can_usb);
    json_bool(&json, "canStartWifi", can_wifi);
    json_bool(&json, "canStartBoth", can_usb && can_wifi);
    json_text(&json, "usbError", usb_error);
    json_text(&json, "wifiError", wifi_error);
    json_text(&json, "detail", detail);
    json_text(&json, "error", error);
    json_append(&json, "}\n");
    sn_quiet(was_quiet);
    if (json.failed) {
        out[0] = 0;
        return -1;
    }
    return 0;
}

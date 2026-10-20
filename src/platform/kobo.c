#include "access/access.h"
#include "network/network.h"
#include "platform/kobo_lock.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define GADGET "/sys/kernel/config/usb_gadget/g1"
#define ECM GADGET "/functions/ecm.supernet"
#define LINK GADGET "/configs/c.1/ecm.supernet"
#define USB_HOOK "/usr/local/Kobo/udev/usb"
#define FORMA_MODULES "/drivers/mx6sll-ntx/usb/gadget/"

static const char *attributes[] = {"idVendor",
                                   "idProduct",
                                   "strings/0x409/serialnumber",
                                   "strings/0x409/manufacturer",
                                   "strings/0x409/product",
                                   "configs/c.1/strings/0x409/configuration",
                                   NULL};

static bool exists(const char *path)
{
    struct stat st;
    return !lstat(path, &st);
}

static int mark(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), SN_RUN "/%s", name);
    return sn_write_text(path, "1\n");
}

static bool module_loaded(const char *module)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/module/%s", module);
    return exists(path);
}

static const char *forma_modules[] = {"configfs", "libcomposite", "u_ether", "usb_f_ecm", NULL};

enum kobo_model { KOBO_UNKNOWN, KOBO_ELIPSA_2E, KOBO_FORMA };

static enum kobo_model device_model(void)
{
    char device[64];
    if (sn_read_text(SN_SDK_ROOT "/control/device", device, sizeof(device))) {
        return KOBO_UNKNOWN;
    }
    if (!strcmp(device, "elipsa2e")) {
        return KOBO_ELIPSA_2E;
    }
    if (!strcmp(device, "forma")) {
        return KOBO_FORMA;
    }
    return KOBO_UNKNOWN;
}

static bool own_guard(void)
{
    struct stat source, target;
    return exists(SN_RUN "/guard") && !stat(SN_RUN "/usb-guard", &source) &&
           !stat(USB_HOOK, &target) && source.st_dev == target.st_dev &&
           source.st_ino == target.st_ino;
}

static bool own_ecm_link(void)
{
    /* configfs returns a relative link regardless of the path supplied to
     * symlink(). Compare resolved identity, never its textual spelling. */
    char *target = realpath(LINK, NULL);
    bool owned = target && !strcmp(target, ECM);
    free(target);
    return owned;
}

static bool usb_conflict(bool allow_own_guard)
{
    if (exists("/var/run/eink-sdk/usbnet") ||
        (sn_is_mounted(USB_HOOK) && !(allow_own_guard && own_guard())) ||
        module_loaded("g_ether") || module_loaded("g_file_storage") ||
        module_loaded("g_mass_storage")) {
        return true;
    }
    glob_t paths = {0};
    glob("/sys/kernel/config/usb_gadget/*", 0, NULL, &paths);
    bool conflict = false;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        char path[512], value[128];
        snprintf(path, sizeof(path), "%s/UDC", paths.gl_pathv[i]);
        if (strcmp(paths.gl_pathv[i], GADGET) || sn_read_text(path, value, sizeof(value)) ||
            *value) {
            conflict = true;
        }
    }
    globfree(&paths);
    if (exists(ECM) || exists(GADGET "/functions/ecm.usb0")) {
        conflict = true;
    }
    glob(GADGET "/configs/*/*", 0, NULL, &paths);
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        struct stat st;
        char target[512];
        if (lstat(paths.gl_pathv[i], &st) || !S_ISLNK(st.st_mode)) {
            continue;
        }
        ssize_t len = readlink(paths.gl_pathv[i], target, sizeof(target) - 1);
        if (len < 0) {
            conflict = true;
            continue;
        }
        target[len] = 0;
        const char *name = strrchr(target, '/');
        if (!name || strncmp(name + 1, "mass_storage.", 13)) {
            conflict = true;
        }
    }
    globfree(&paths);
    return conflict;
}

enum sn_usb_state sn_usb_state(const struct config *cfg, char *detail, size_t size)
{
    enum kobo_model model = device_model();
    if (model == KOBO_UNKNOWN) {
        snprintf(detail,
                 size,
                 "This Supernet build supports Kobo Forma and Elipsa 2E USB controllers.");
        return SN_USB_UNAVAILABLE;
    }
    if (!sn_is_mounted("/mnt/onboard")) {
        snprintf(detail, size, "Eject USB storage and wait for Kobo storage to mount.");
        return SN_USB_UNAVAILABLE;
    }
    if (sn_usb_owned(cfg)) {
        char expected[128], actual[128];
        if (own_ecm_link() && !sn_read_text(SN_RUN "/bound", expected, sizeof(expected)) &&
            !sn_read_text(GADGET "/UDC", actual, sizeof(actual)) && *actual &&
            !strcmp(actual, expected) && sn_interface(cfg, false)) {
            return SN_USB_OWNED;
        }
        snprintf(detail,
                 size,
                 "Supernet has unfinished or changed USB resources. Stop SSH to release what it "
                 "owns; reboot if cleanup is blocked.");
        return SN_USB_INCOMPLETE;
    }
    if (exists(SN_RUN "/network")) {
        snprintf(detail,
                 size,
                 "Supernet's configuration changed while USB access was reserved. Restore it or "
                 "reboot.");
        return SN_USB_UNAVAILABLE;
    }
    if (usb_conflict(false) || sn_interface(cfg, false)) {
        snprintf(detail,
                 size,
                 "USB networking belongs to the bootstrap or another gadget. Supernet will not "
                 "take it over.");
        return SN_USB_FOREIGN;
    }
    if (sn_kobo_locked()) {
        snprintf(detail, size, "The USB controller is busy.");
        return SN_USB_UNAVAILABLE;
    }
    if (model == KOBO_FORMA) {
        for (unsigned i = 0; forma_modules[i]; i++) {
            char path[256];
            snprintf(path, sizeof(path), FORMA_MODULES "%s.ko", forma_modules[i]);
            if (!module_loaded(forma_modules[i]) && access(path, R_OK)) {
                snprintf(detail,
                         size,
                         "The Forma firmware USB module %s is unavailable.",
                         forma_modules[i]);
                return SN_USB_UNAVAILABLE;
            }
        }
    }
    return SN_USB_IDLE;
}

static int make_directory(const char *path)
{
    if (!mkdir(path, 0755) || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int load_forma_modules(void)
{
    for (unsigned i = 0; forma_modules[i]; i++) {
        if (module_loaded(forma_modules[i])) {
            continue;
        }
        char path[256], marker[128];
        snprintf(path, sizeof(path), FORMA_MODULES "%s.ko", forma_modules[i]);
        snprintf(marker, sizeof(marker), "module-%s", forma_modules[i]);
        /* Record intent first, then load only the previously absent module.
         * A failed or interrupted insmod remains safe to unwind. */
        if (mark(marker)) {
            return -1;
        }
        char *args[] = {"/sbin/insmod", path, NULL};
        if (sn_run(args, 5000)) {
            return -1;
        }
    }
    return 0;
}

static int unload_forma_modules(void)
{
    for (unsigned i = 4; i > 0; i--) {
        const char *module = forma_modules[i - 1];
        char marker[256];
        snprintf(marker, sizeof(marker), SN_RUN "/module-%s", module);
        if (!exists(marker)) {
            continue;
        }
        if (module_loaded(module)) {
            char *args[] = {"/sbin/rmmod", (char *)module, NULL};
            if (sn_run(args, 5000)) {
                return -1;
            }
        }
        if (unlink(marker)) {
            return -1;
        }
    }
    return 0;
}

static int restore_gadget(const struct config *cfg)
{
    if (!sn_usb_owned(cfg)) {
        if (exists(SN_RUN "/network")) {
            sn_log(LOG_ERR, "configuration differs from owned USB state; refusing cleanup");
            return -1;
        }
        return 0;
    }
    if (exists(SN_RUN "/bound")) {
        char expected[128], actual[128];
        if (!own_ecm_link() || sn_read_text(SN_RUN "/bound", expected, sizeof(expected)) ||
            sn_read_text(GADGET "/UDC", actual, sizeof(actual)) ||
            (*actual && strcmp(actual, expected))) {
            sn_log(LOG_ERR, "USB gadget ownership changed; refusing to detach another controller");
            return -1;
        }
        if (*actual && sn_write_text(GADGET "/UDC", "\n")) {
            return -1;
        }
        if (unlink(SN_RUN "/bound")) {
            return -1;
        }
    }
    if (exists(SN_RUN "/ecm-link")) {
        if (unlink(LINK) && errno != ENOENT) {
            return -1;
        }
        if (unlink(SN_RUN "/ecm-link")) {
            return -1;
        }
    }
    if (exists(SN_RUN "/ecm-function")) {
        if (rmdir(ECM) && errno != ENOENT) {
            return -1;
        }
        if (unlink(SN_RUN "/ecm-function")) {
            return -1;
        }
    }
    for (unsigned i = 0; attributes[i]; i++) {
        char backup[256], path[512], value[256];
        snprintf(backup, sizeof(backup), SN_RUN "/attribute-%u", i);
        if (!exists(backup)) {
            continue;
        }
        snprintf(path, sizeof(path), GADGET "/%s", attributes[i]);
        if (sn_read_text(backup, value, sizeof(value)) || sn_write_text(path, value)) {
            return -1;
        }
        if (unlink(backup)) {
            return -1;
        }
    }
    FILE *links = fopen(SN_RUN "/storage-links", "r");
    if (links) {
        char path[512], target[512];
        int rc = 0;
        while (fscanf(links, "%511s %511s", path, target) == 2) {
            if (!exists(path) && symlink(target, path)) {
                rc = -1;
                break;
            }
        }
        fclose(links);
        if (rc || unlink(SN_RUN "/storage-links")) {
            return -1;
        }
    }
    char model[16];
    if (sn_read_text(SN_RUN "/device", model, sizeof(model))) {
        return -1;
    }
    if (!strcmp(model, "forma")) {
        if (exists(SN_RUN "/created-gadget")) {
            const char *directories[] = {GADGET "/configs/c.1/strings/0x409",
                                         GADGET "/configs/c.1",
                                         GADGET "/strings/0x409",
                                         GADGET,
                                         NULL};
            for (unsigned i = 0; directories[i]; i++) {
                if (rmdir(directories[i]) && errno != ENOENT) {
                    return -1;
                }
            }
            if (unlink(SN_RUN "/created-gadget")) {
                return -1;
            }
        }
        if (exists(SN_RUN "/mounted-configfs")) {
            if (sn_is_mounted("/sys/kernel/config") && umount("/sys/kernel/config")) {
                return -1;
            }
            if (unlink(SN_RUN "/mounted-configfs")) {
                return -1;
            }
        }
        if (unload_forma_modules()) {
            return -1;
        }
    } else if (strcmp(model, "elipsa2e")) {
        sn_log(LOG_ERR, "unknown USB owner model; refusing cleanup");
        return -1;
    }
    /* MediaTek permits one gadget only, so Elipsa retains an unbound g1 for
     * firmware use. Forma releases its created gadget and loaded modules.
     * Neither backend binds storage or changes the userstore mount. */
    if (exists(SN_RUN "/guard")) {
        if (sn_is_mounted(USB_HOOK)) {
            if (!own_guard()) {
                sn_log(LOG_ERR,
                       "USB storage guard belongs to another tool; refusing to unmount it");
                return -1;
            }
            if (umount(USB_HOOK)) {
                return -1;
            }
        }
        if (unlink(SN_RUN "/guard")) {
            return -1;
        }
    }
    unlink(SN_RUN "/usb-guard");
    unlink(SN_RUN "/created-gadget");
    if (exists(SN_RUN "/mounted-configfs")) {
        /* A new unbound g1 keeps configfs busy; it is safe to leave mounted. */
        unlink(SN_RUN "/mounted-configfs");
    }
    if (unlink(SN_RUN "/network")) {
        return -1;
    }
    return unlink(SN_RUN "/device");
}

int sn_usb_stop(const struct config *cfg)
{
    if (!sn_usb_owned(cfg)) {
        return exists(SN_RUN "/network") ? -1 : 0;
    }
    int lock = sn_kobo_lock();
    if (lock < 0) {
        return -1;
    }
    int rc = restore_gadget(cfg);
    if (sn_kobo_unlock(lock)) {
        rc = -1;
    }
    if (rc) {
        sn_log(LOG_ERR, "USB cleanup is incomplete; retry Stop SSH or reboot");
    }
    return rc;
}

enum carrier_phase {
    WAITING_FOR_HOST,
    REOPEN_INTERFACE,
    CONFIGURE_INTERFACE,
    HOST_READY,
};
static enum carrier_phase carrier_phase;

int sn_usb_poll(const struct config *cfg)
{
    if (!sn_usb_owned(cfg)) {
        return 0;
    }
    if (carrier_phase == WAITING_FOR_HOST || carrier_phase == HOST_READY) {
        char path[128], value[16], detail[256] = "";
        snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", cfg->interface);
        if (sn_read_text(path, value, sizeof(value))) {
            return 0;
        }
        if (!strcmp(value, "0")) {
            carrier_phase = WAITING_FOR_HOST;
            return 0;
        }
        if (strcmp(value, "1") || carrier_phase == HOST_READY) {
            return 0;
        }
        if (sn_usb_state(cfg, detail, sizeof(detail)) != SN_USB_OWNED) {
            sn_log(LOG_ERR, "%s", detail);
            return -1;
        }
        /* Forma starts RX/TX on SET_INTERFACE. Only Elipsa needs a reopen. */
        carrier_phase = device_model() == KOBO_ELIPSA_2E ? REOPEN_INTERFACE : HOST_READY;
    }
    if (carrier_phase == REOPEN_INTERFACE) {
        if (sn_net_down(cfg)) {
            sn_log(LOG_ERR, "reopen %s after USB connection: %s", cfg->interface, strerror(errno));
            return -1;
        }
        carrier_phase = CONFIGURE_INTERFACE;
    }
    if (carrier_phase == CONFIGURE_INTERFACE) {
        /* After a failed configure the interface can be down. Retry this step
         * without waiting for carrier, which itself requires the interface up. */
        if (sn_net_configure(cfg)) {
            return -1;
        }
        carrier_phase = HOST_READY;
    }
    sn_log(LOG_INFO, "USB host connected; %s is ready", cfg->interface);
    return 0;
}

bool sn_usb_needs_cleanup(enum sn_usb_state state)
{
    return state == SN_USB_OWNED || state == SN_USB_INCOMPLETE;
}

const char *sn_usb_stopped_detail(enum sn_usb_state state)
{
    (void)state;
    return NULL;
}

int sn_usb_start(const struct config *cfg)
{
    char detail[256] = "";
    enum sn_usb_state state = sn_usb_state(cfg, detail, sizeof(detail));
    if (!sn_usb_can_start(state)) {
        sn_log(LOG_ERR, "%s", detail);
        return -1;
    }
    carrier_phase = WAITING_FOR_HOST;
    int lock = sn_kobo_lock();
    if (lock < 0) {
        return -1;
    }
    int rc = -1;
    if (state == SN_USB_OWNED) {
        rc = sn_interface(cfg, false) ? sn_net_configure(cfg) : -1;
        if (sn_kobo_unlock(lock)) {
            rc = -1;
        }
        return rc;
    }
    /* Recheck after taking the bootstrap's existing lock. */
    enum kobo_model model = device_model();
    if (model == KOBO_UNKNOWN || usb_conflict(false) ||
        sn_write_text(SN_RUN "/device", model == KOBO_FORMA ? "forma\n" : "elipsa2e\n") ||
        sn_usb_mark_owned(cfg)) {
        goto done;
    }
    if (sn_write_text(SN_RUN "/usb-guard", "#!/bin/sh\nexit 0\n") ||
        chmod(SN_RUN "/usb-guard", 0755) || mark("guard") ||
        mount(SN_RUN "/usb-guard", USB_HOOK, NULL, MS_BIND, NULL)) {
        goto done;
    }
    if (model == KOBO_FORMA) {
        if (load_forma_modules()) {
            goto done;
        }
    } else if (!module_loaded("usb_f_ecm")) {
        char *args[] = {SN_SDK_ROOT "/control/modprobe", "usb_f_ecm", NULL};
        if (sn_run(args, 5000)) {
            goto done;
        }
    }
    if (!sn_is_mounted("/sys/kernel/config")) {
        if (mark("mounted-configfs") ||
            mount("configfs", "/sys/kernel/config", "configfs", 0, NULL)) {
            goto done;
        }
    }
    /* A gadget hidden by an unmounted configfs still belongs to its creator. */
    if (usb_conflict(true)) {
        goto done;
    }
    bool created = !exists(GADGET);
    if (created && (mark("created-gadget") || make_directory(GADGET))) {
        goto done;
    }
    if (!created) {
        for (unsigned i = 0; attributes[i]; i++) {
            char backup[256], path[512], value[256];
            snprintf(path, sizeof(path), GADGET "/%s", attributes[i]);
            if (!exists(path)) {
                continue;
            }
            snprintf(backup, sizeof(backup), SN_RUN "/attribute-%u", i);
            if (sn_read_text(path, value, sizeof(value)) || sn_write_text(backup, value)) {
                goto done;
            }
        }
    }
    if (make_directory(GADGET "/strings/0x409") || make_directory(GADGET "/configs/c.1") ||
        make_directory(GADGET "/configs/c.1/strings/0x409")) {
        goto done;
    }
    FILE *links = fopen(SN_RUN "/storage-links", "w");
    if (!links) {
        goto done;
    }
    glob_t paths = {0};
    glob(GADGET "/configs/*/*", 0, NULL, &paths);
    bool failed = false;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        struct stat st;
        if (lstat(paths.gl_pathv[i], &st) || !S_ISLNK(st.st_mode)) {
            continue;
        }
        char *target = realpath(paths.gl_pathv[i], NULL);
        const char *name = target ? strrchr(target, '/') : NULL;
        if (!name || strncmp(name + 1, "mass_storage.", 13) ||
            fprintf(links, "%s %s\n", paths.gl_pathv[i], target) < 0 || fflush(links) ||
            unlink(paths.gl_pathv[i])) {
            failed = true;
        }
        free(target);
        if (failed) {
            break;
        }
    }
    globfree(&paths);
    if (fclose(links) || failed) {
        goto done;
    }
    char serial[128];
    if (sn_read_text("/mnt/onboard/.kobo/version", serial, sizeof(serial))) {
        goto done;
    }
    serial[strcspn(serial, ",")] = 0;
    const char *values[] = {"0x0525", "0xa4a1", serial, "Supernet", "Kobo USB Ethernet", "CDC ECM"};
    for (unsigned i = 0; attributes[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), GADGET "/%s", attributes[i]);
        if (sn_write_text(path, values[i])) {
            goto done;
        }
    }
    if (mark("ecm-function") || mkdir(ECM, 0755)) {
        goto done;
    }
    /* Stable, locally administered addresses; no DHCP/router/DNS changes. */
    unsigned hash = 2166136261u;
    for (unsigned char *p = (unsigned char *)serial; *p; p++) {
        hash = (hash ^ *p) * 16777619u;
    }
    char host[32], device[32];
    snprintf(host,
             sizeof(host),
             "ee:49:00:%02x:%02x:%02x",
             (hash >> 16) & 255,
             (hash >> 8) & 255,
             hash & 255);
    snprintf(device,
             sizeof(device),
             "ee:19:00:%02x:%02x:%02x",
             (hash >> 16) & 255,
             (hash >> 8) & 255,
             hash & 255);
    if (sn_write_text(ECM "/host_addr", host) || sn_write_text(ECM "/dev_addr", device) ||
        mark("ecm-link") || symlink(ECM, LINK)) {
        goto done;
    }
    glob("/sys/class/udc/*", 0, NULL, &paths);
    if (paths.gl_pathc != 1) {
        globfree(&paths);
        goto done;
    }
    const char *controller = strrchr(paths.gl_pathv[0], '/');
    int bound = controller ? sn_write_text(SN_RUN "/bound", controller + 1) : -1;
    if (!bound) {
        bound = sn_write_text(GADGET "/UDC", controller + 1);
    }
    globfree(&paths);
    if (bound) {
        goto done;
    }
    char interface[IFNAMSIZ];
    if (sn_read_text(ECM "/ifname", interface, sizeof(interface)) ||
        strcmp(interface, cfg->interface) || sn_net_configure(cfg)) {
        goto done;
    }
    /* A listener can bind an addressed USB interface before a host connects.
     * Keep Wi-Fi immediately usable in Both mode; the daemon handles a later
     * USB carrier transition without blocking command processing. */
    if (sn_usb_poll(cfg)) {
        goto done;
    }
    rc = 0;
done:
    if (rc) {
        sn_log(LOG_ERR,
               "%s",
               *sn_error() ? sn_error() : "Could not prepare the Kobo USB network interface.");
    }
    if (rc && sn_usb_owned(cfg) && restore_gadget(cfg)) {
        sn_log(LOG_ERR, "USB cleanup is incomplete; retry Stop SSH or reboot");
    }
    if (sn_kobo_unlock(lock)) {
        rc = -1;
    }
    return rc;
}

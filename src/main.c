#include "access/access.h"
#include "control/control.h"
#include "network/network.h"
#include "platform/usb.h"
#include "runtime/runtime.h"
#include "supernet.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        puts("Usage: supernet "
             "--version|status-json|start-usb|start-wifi|start-both|stop|adb-start|adb-stop|awake-"
             "on|awake-off|prepare-remove|shutdown");
        return 2;
    }
    const char *command = argv[1];
    if (!strcmp(command, "--version")) {
        puts("supernet " SN_VERSION " (" SN_PLATFORM ")");
        return 0;
    }
    if (!strcmp(command, "status-json")) {
        sn_quiet(true);
        int rc = sn_request(command, true, true);
        if (rc >= 0) {
            return rc;
        }
        char result[SN_REPLY_SIZE];
        int owned = sn_ssh_processes(0);
        struct sn_service_view view = {.ssh_running = owned > 0};
        if (owned < 0 || sn_status(result, sizeof(result), &view)) {
            fputs("Could not read Supernet status.\n", stderr);
            return 1;
        }
        fputs(result, stdout);
        return 0;
    }
    umask(0077);
    openlog("supernet", LOG_PID, LOG_LOCAL4);
    if (geteuid()) {
        fputs("Supernet controls require root.\n", stderr);
        return 1;
    }
    if (!strcmp(command, "adb-start") || !strcmp(command, "adb-stop") ||
        !strcmp(command, "awake-on") || !strcmp(command, "awake-off")) {
        int rc = sn_request(command, true, true);
        if (rc >= 0) {
            return rc;
        }
        if (!strcmp(command, "awake-off")) {
            return 0;
        }
        if (!strcmp(command, "adb-stop")) {
            if (sn_adb_set(false)) {
                return 1;
            }
            puts("OK adb-stop");
            return 0;
        }
        if (sn_bootstrap()) {
            return 1;
        }
        return sn_request(command, false, true);
    }
    if (!strcmp(command, "daemon") || !strcmp(command, "serve")) {
        return sn_serve(!strcmp(command, "daemon"));
    }
    if (!strcmp(command, "start-usb") || !strcmp(command, "start-wifi") ||
        !strcmp(command, "start-both")) {
        struct config cfg;
        if (sn_config(SN_CONFIG, &cfg)) {
            return 1;
        }
        cfg.usb = strcmp(command, "start-wifi") != 0;
        cfg.wifi = strcmp(command, "start-usb") != 0;
        int rc = sn_request("status-json", true, false);
        if (rc == 1) {
            return 1;
        }
        if (rc < 0 && (sn_access_check(&cfg, false) || sn_bootstrap())) {
            return 1;
        }
        return sn_request(command, false, true);
    }
    if (!strcmp(command, "stop") || !strcmp(command, "shutdown") ||
        !strcmp(command, "prepare-remove")) {
        int rc = sn_request(command, true, true);
        if (rc >= 0) {
            return rc;
        }
        if (!strcmp(command, "stop") && sn_package_busy()) {
            fputs("A package operation is running.\n", stderr);
            return 1;
        }
        if (!strcmp(command, "prepare-remove") && sn_ssh_processes(0) != 0) {
            fputs("Owned SSH processes remain. Stop Supernet before removing it.\n", stderr);
            return 1;
        }
        int failed = 0;
        if (strcmp(command, "stop") && sn_adb_set(false)) {
            failed = 1;
        }
        if (sn_ssh_recover()) {
            failed = 1;
        }
        if (sn_usb_recover()) {
            failed = 1;
        }
        return failed;
    }
    puts("Unknown Supernet command.");
    return 2;
}

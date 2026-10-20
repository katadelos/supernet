#ifndef SUPERNET_ACCESS_H
#define SUPERNET_ACCESS_H

#include "supernet.h"

bool sn_keys_ready(void);
int sn_keys_prepare(void);
bool sn_package_busy(void);
const char *sn_access_problem(const struct config *cfg, bool ssh_running);
int sn_access_check(const struct config *cfg, bool ssh_running);
int sn_ssh_processes(int signal_number);
int sn_ssh_recover(void);

struct sn_adb_status {
    bool available;
    bool running;
    const char *error;
};
struct sn_adb_status sn_adb_status(bool usb_ssh);
bool sn_adb_running(void);
void sn_adb_reap(void);
int sn_adb_log_fd(void);
int sn_adb_set(bool enabled);

bool sn_wake_available(void);
void sn_wake_update(bool active);
bool sn_wake_held(void);
bool sn_awake_requested(void);
int sn_awake_set(bool enabled);

#endif

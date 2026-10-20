#ifndef SUPERNET_NETWORK_H
#define SUPERNET_NETWORK_H

#include "supernet.h"

bool sn_interface(const struct config *cfg, bool require_up);
int sn_address(const char *name, char out[16]);
int sn_net_configure(const struct config *cfg);
int sn_net_down(const struct config *cfg);
int sn_wifi_address(char interface[IFNAMSIZ], char address[16]);
bool sn_firewall_pending(void);
int sn_firewall_set(const struct config *cfg, const char *interface, const char *address, bool add);

#endif

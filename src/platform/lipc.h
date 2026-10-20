#ifndef SUPERNET_LIPC_H
#define SUPERNET_LIPC_H

int sn_lipc_get(int *value);
int sn_lipc_set(int value);
int sn_lipc_event(const char *name);

#endif

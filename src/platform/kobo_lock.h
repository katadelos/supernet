#ifndef SUPERNET_KOBO_LOCK_H
#define SUPERNET_KOBO_LOCK_H

#include <stdbool.h>

bool sn_kobo_locked(void);
int sn_kobo_lock(void);
int sn_kobo_unlock(int fd);

#endif

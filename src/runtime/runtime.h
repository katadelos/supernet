#ifndef SUPERNET_RUNTIME_H
#define SUPERNET_RUNTIME_H

#include "supernet.h"

void sn_log(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool sn_quiet(bool value);
void sn_error_clear(void);
const char *sn_error(void);
/* Each runtime log discards older contents before an append exceeds this cap. */
#define SN_LOG_LIMIT (256U * 1024U)
void sn_log_append(const char *path, const char *data, size_t length);
int sn_log_pipe(int pipefd[2]);
void sn_log_drain(int fd, const char *program);

uint64_t sn_now(void);
void sn_sleep(unsigned ms);
int sn_deadline_remaining(uint64_t deadline);
/* Count matching live processes, optionally signalling them; -1 on failure.
 * NULL executable matches any executable; parent 0 matches any parent. */
int sn_processes(const char *executable, pid_t parent, int signal_number);
int sn_processes_stop(const char *executable, pid_t parent);
int sn_process_wait(pid_t pid, unsigned timeout_ms);
int sn_run(char *const argv[], unsigned timeout_ms);

bool sn_config_network_valid(const struct config *cfg);
int sn_config(const char *path, struct config *cfg);
int sn_mkdir(const char *path, mode_t mode);
int sn_copy(const char *source, const char *destination, mode_t mode);
int sn_read_text(const char *path, char *out, size_t size);
int sn_write_text(const char *path, const char *value);
bool sn_is_mounted(const char *path);
int sn_stage(void);
int sn_bootstrap(void);

#endif

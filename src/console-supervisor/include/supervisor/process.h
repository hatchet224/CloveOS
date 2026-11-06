#ifndef SUPERVISOR_PROCESS_H
#define SUPERVISOR_PROCESS_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
bool sc_spawn(const char *path, char *const argv[], int control, int hook,
              const char *bridge_launch, pid_t *pid);
bool sc_spawn_game(const char *path, char *const argv[], const char *bridge_launch,
                   uint64_t appearance, uint32_t shortcuts, pid_t *pid);
struct cl_file_explorer_config;
bool sc_spawn_file_explorer(const struct cl_file_explorer_config *config, pid_t *pid);
struct cl_ports_config;
bool sc_spawn_portmaster(const struct cl_ports_config *config, pid_t *pid);
bool sc_term_once(pid_t pid, bool *sent);
bool sc_shutdown_record(bool create);
#endif

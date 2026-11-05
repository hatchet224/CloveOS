#ifndef SUPERVISOR_GLO_SCRIPTS_H
#define SUPERVISOR_GLO_SCRIPTS_H
#include "supervisor/config.h"
void sc_glo_scripts(const struct sc_config *config, const struct cl_tool_request *request,
                    struct cl_tool_snapshot *snapshot);
int sc_glo_open_script(const struct sc_config *config, const struct sc_game *game,
                       enum cl_glo_scope scope, const char *key);
#endif

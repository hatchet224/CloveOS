#ifndef SC_CORE_CHOICE_H
#define SC_CORE_CHOICE_H
#include "supervisor/config.h"
bool sc_core_resolve(const struct sc_config *config, const char game[65], struct sc_game *effective);
void sc_core_options(const struct sc_config *config, const struct cs_store *store,
    const struct cl_tool_request *request, struct cl_tool_snapshot *snapshot);
#endif

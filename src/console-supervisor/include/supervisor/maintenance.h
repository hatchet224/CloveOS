#ifndef SUPERVISOR_MAINTENANCE_H
#define SUPERVISOR_MAINTENANCE_H
#include "supervisor/config.h"
void sc_maintenance_request(const struct sc_config *config, const struct cs_store *store,
                            enum cl_tool_operation operation, struct cl_tool_snapshot *result);
#endif

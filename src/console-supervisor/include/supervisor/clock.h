#ifndef SC_CLOCK_H
#define SC_CLOCK_H
#include "launcher/tools.h"
struct cs_store;
void sc_clock_request(const struct cs_store *store,const struct cl_tool_request *request,struct cl_tool_snapshot *snapshot);
#endif

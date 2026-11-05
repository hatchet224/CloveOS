#ifndef SUPERVISOR_NETWORK_H
#define SUPERVISOR_NETWORK_H
#include "launcher/tools.h"
#include "sessiond/store.h"
/* Storage-worker only. Fixed local wpa_supplicant endpoint; no shell or host
 * networking. Association is asynchronous and never treated as an IP lease. */
void sc_network_request(const struct cs_store *store, const struct cl_tool_request *request,
                         struct cl_tool_snapshot *snapshot);
bool sc_network_reset(const struct cs_store *store);
void sc_device_snapshot(const struct cs_store *store, struct cl_tool_snapshot *snapshot);
#endif

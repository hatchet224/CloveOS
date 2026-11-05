#ifndef SC_LAN_SERVICES_H
#define SC_LAN_SERVICES_H

#include "launcher/tools.h"
#include <netinet/in.h>
#include <sys/types.h>

struct sc_lan_endpoint {
    pid_t pid;
    int control;
    bool wanted, ready, stopping, failed;
    uint64_t deadline;
    char password[13], fingerprint[52];
};

struct sc_lan_services {
    struct sc_lan_endpoint endpoints[2];
    char address[INET_ADDRSTRLEN];
    uint64_t next_scan;
};

void sc_lan_init(struct sc_lan_services *services);
void sc_lan_update(struct sc_lan_services *services, const char *card, uint64_t now, bool paused);
bool sc_lan_busy(const struct sc_lan_services *services);
bool sc_lan_reap(struct sc_lan_services *services, pid_t pid, bool success);
void sc_lan_request(struct sc_lan_services *services, const char *card, uint64_t now, bool paused,
                    enum cl_tool_operation operation, struct cl_tool_snapshot *out);

#endif

#ifndef SESSIOND_BRIDGE_H
#define SESSIOND_BRIDGE_H
#include "sessiond/fsm.h"
#include "sessiond/store.h"
#include <sys/types.h>
#define CS_BRIDGE_CAPABILITIES 63U
enum cs_bridge_status {
    CS_BRIDGE_OK,
    CS_BRIDGE_FAILED,
    CS_BRIDGE_UNSUPPORTED,
    CS_BRIDGE_UNCERTAIN,
    CS_BRIDGE_PROTOCOL,
    CS_BRIDGE_CANCELLED
};
struct cs_bridge_reply {
    enum cs_bridge_status status;
    uint64_t frame;
    bool writers_closed;
};
struct cs_bridge {
    int fd;
    uid_t expected_uid;
    char path[108], instance[33], data_root[CS_PATH_MAX];
    struct cs_identity identity;
    char receive[8192];
    size_t received;
    bool hello;
    bool (*stopping)(void *context);
    void *context;
};
bool cs_bridge_init(struct cs_bridge *bridge, const char *path, uid_t expected_uid,
                    const char *instance, const char *data_root,
                    const struct cs_identity *identity);
void cs_bridge_close(struct cs_bridge *bridge);
struct cs_bridge_reply cs_bridge_exchange(struct cs_bridge *bridge,
                                          const struct cs_request *request, const char *directory,
                                          const struct cs_request *target, uint64_t timeout_ms);
const char *cs_bridge_command(enum cs_operation operation);
#endif

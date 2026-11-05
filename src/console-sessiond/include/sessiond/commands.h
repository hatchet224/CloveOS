#ifndef SESSIOND_COMMANDS_H
#define SESSIOND_COMMANDS_H

#include "sessiond/fsm.h"

enum cs_command_kind {
    CS_CMD_EVENT,
    CS_CMD_PRESS,
    CS_CMD_RELEASE,
    CS_CMD_REPEAT,
    CS_CMD_WAKE,
    CS_CMD_FAIL,
    CS_CMD_DROP,
    CS_CMD_STATUS,
    CS_CMD_QUIT,
    CS_CMD_EXPECT
};
struct cs_command {
    enum cs_command_kind kind;
    enum cs_event_kind event;
    enum cs_operation operation;
    enum cs_state expected_state;
};

/* Exact, bounded command language; never evaluated by a shell. */
bool cs_command_parse(const char *line, struct cs_command *command);

#endif

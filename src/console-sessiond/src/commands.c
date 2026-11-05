#include "sessiond/commands.h"

#include <stddef.h>
#include <string.h>

struct command_entry {
    const char *text;
    enum cs_command_kind kind;
    enum cs_event_kind event;
};

bool cs_command_parse(const char *line, struct cs_command *command)
{
    static const struct command_entry entries[] = {
        {"launch", CS_CMD_EVENT, CS_EV_LAUNCH},
        {"resume", CS_CMD_EVENT, CS_EV_RESUME},
        {"menu", CS_CMD_EVENT, CS_EV_MENU},
        {"retry", CS_CMD_EVENT, CS_EV_RETRY},
        {"continue", CS_CMD_EVENT, CS_EV_CONTINUE},
        {"confirm-off", CS_CMD_EVENT, CS_EV_CONFIRM_OFF},
        {"recovery-menu", CS_CMD_EVENT, CS_EV_RECOVERY_MENU},
        {"critical", CS_CMD_EVENT, CS_EV_CRITICAL_BATTERY},
        {"crash", CS_CMD_EVENT, CS_EV_EMULATOR_EXIT},
        {"press", CS_CMD_PRESS, CS_EV_TICK},
        {"release", CS_CMD_RELEASE, CS_EV_TICK},
        {"repeat", CS_CMD_REPEAT, CS_EV_TICK},
        {"wake", CS_CMD_WAKE, CS_EV_TICK},
        {"status", CS_CMD_STATUS, CS_EV_TICK},
        {"quit", CS_CMD_QUIT, CS_EV_TICK}};
    size_t i;
    if (line == NULL || command == NULL) {
        return false;
    }
    memset(command, 0, sizeof(*command));
    for (i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (strcmp(line, entries[i].text) == 0) {
            command->kind = entries[i].kind;
            command->event = entries[i].event;
            return true;
        }
    }
    if (strncmp(line, "fail ", 5U) == 0 || strncmp(line, "drop ", 5U) == 0) {
        for (i = 1U; i < (size_t)CS_OP_COUNT; ++i) {
            if (strcmp(line + 5, cs_operation_name((enum cs_operation)i)) == 0) {
                command->kind = line[0] == 'f' ? CS_CMD_FAIL : CS_CMD_DROP;
                command->operation = (enum cs_operation)i;
                return true;
            }
        }
    }
    if (strncmp(line, "expect ", 7U) == 0) {
        for (i = 0U; i < (size_t)CS_STATE_COUNT; ++i) {
            if (strcmp(line + 7, cs_state_name((enum cs_state)i)) == 0) {
                command->kind = CS_CMD_EXPECT;
                command->expected_state = (enum cs_state)i;
                return true;
            }
        }
    }
    return false;
}

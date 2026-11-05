#ifndef SUPERVISOR_DIAGNOSTIC_POLICY_H
#define SUPERVISOR_DIAGNOSTIC_POLICY_H
#include "sessiond/fsm.h"
static inline bool sc_diagnostics_can_export(enum cs_state state, bool active)
{
    return !active || state == CS_QUICK_MENU || state == CS_RECOVERY ||
           state == CS_CHECKPOINT_ERROR;
}
#endif

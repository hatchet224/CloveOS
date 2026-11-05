#ifndef SUPERVISOR_MENU_OBSERVER_H
#define SUPERVISOR_MENU_OBSERVER_H
#include "sessiond/hotkeys.h"

/* Call only after draining the event queue and outside SYN_DROPPED recovery.
 * The key bitmap is a fresh physical observation, not a replayed event. Keep
 * an ordering barrier so the queued copy of this edge cannot trigger twice. */
static inline bool sc_menu_reconcile(struct cs_hotkeys *keys,bool held,
                                     uint64_t now,uint64_t *sampled_at)
{
    if (held==keys->down || now<*sampled_at) return false;
    cs_hotkeys_menu(keys,held,false,now);
    *sampled_at=now;
    return true;
}
#endif

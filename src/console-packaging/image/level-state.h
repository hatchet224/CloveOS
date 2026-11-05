#ifndef CONSOLE_LEVEL_STATE_H
#define CONSOLE_LEVEL_STATE_H
#include "sessiond/store.h"
struct console_levels {
    uint64_t revision;
    unsigned int brightness; /* 0 = not recorded; otherwise 8..255. */
    unsigned int volume;     /* 101 = not recorded; otherwise UI 0..100, mapped to bounded DAC raw units. */
};
enum cs_store_status console_levels_load(struct cs_store *store, struct console_levels *levels);
bool console_levels_save(struct cs_store *store, int brightness, int volume);
#endif

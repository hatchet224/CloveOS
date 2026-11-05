#ifndef SESSIOND_HOTKEYS_H
#define SESSIOND_HOTKEYS_H
#include <stdbool.h>
#include <stdint.h>
/* Physical MENU observer. A chord consumer suppresses the bare MENU action;
 * repeat, initial held keys, dropped input and clock rollback never create it. */
enum cs_menu_action { CS_MENU_NONE, CS_MENU_RECENT, CS_MENU_LIBRARY, CS_MENU_PREVIOUS };
struct cs_hotkeys {
    uint64_t down_at, release_at;
    bool down, armed, chord, pending, long_press, second_press;
    uint64_t volume_at;
    bool volume_held[2], volume_sent;
};
void cs_hotkeys_reset(struct cs_hotkeys *keys, bool held);
void cs_hotkeys_menu(struct cs_hotkeys *keys, bool down, bool repeat, uint64_t now);
void cs_hotkeys_consume(struct cs_hotkeys *keys);
/* Physical volume edge/repeat gate. A held MENU consumes the bare-menu action
 * even when a repeated adjustment is throttled. Initial repeats are ignored. */
bool cs_hotkeys_volume(struct cs_hotkeys *keys, bool up, int value, uint64_t now);
enum cs_menu_action cs_hotkeys_tick(struct cs_hotkeys *keys, uint64_t now);
#endif

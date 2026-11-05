#include "sessiond/hotkeys.h"
#include <string.h>
void cs_hotkeys_reset(struct cs_hotkeys *k, bool held) {
    memset(k, 0, sizeof(*k));
    k->down = held;
    k->armed = !held;
}
void cs_hotkeys_menu(struct cs_hotkeys *k, bool down, bool repeat, uint64_t now) {
    if (repeat || down == k->down)
        return;
    k->down = down;
    if (down) {
        k->second_press = k->pending && now >= k->down_at && now - k->down_at < 300U;
        k->down_at = now;
        k->chord = false;
        k->pending = false;
    } else {
        if (k->armed && !k->chord && now >= k->down_at) {
            k->release_at = now;
            k->pending = true;
            k->long_press = now - k->down_at >= 700U;
        }
        k->armed = true;
    }
}
void cs_hotkeys_consume(struct cs_hotkeys *k) {
    k->chord = true;
    k->pending = false;
}
bool cs_hotkeys_volume(struct cs_hotkeys *k, bool up, int value, uint64_t now) {
    unsigned int index = up ? 1U : 0U;
    if (value < 0 || value > 2)
        return false;
    if (value == 0) {
        k->volume_held[index] = false;
        return false;
    }
    if (k->down)
        cs_hotkeys_consume(k);
    if (value == 1) {
        if (k->volume_held[index])
            return false;
        k->volume_held[index] = true;
    } else if (!k->volume_held[index]) {
        return false;
    }
    if (k->volume_sent && (now < k->volume_at || now - k->volume_at < 80U))
        return false;
    k->volume_sent = true;
    k->volume_at = now;
    return true;
}
enum cs_menu_action cs_hotkeys_tick(struct cs_hotkeys *k, uint64_t now) {
    if (k->down && k->armed && !k->chord && now >= k->down_at && now - k->down_at >= 700U) {
        cs_hotkeys_consume(k);
        return CS_MENU_LIBRARY;
    }
    if (k->pending && k->second_press && !k->down && !k->chord) {
        k->pending = false;
        k->second_press = false;
        return CS_MENU_PREVIOUS;
    }
    if (!k->pending || k->down || k->chord || now < k->release_at ||
        (!k->long_press && (now < k->down_at || now - k->down_at < 300U)))
        return CS_MENU_NONE;
    k->pending = false;
    return k->long_press ? CS_MENU_LIBRARY : CS_MENU_RECENT;
}

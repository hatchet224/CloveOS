#ifdef NDEBUG
#undef NDEBUG
#endif
#include "configuration.h"
#include "input/input_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "magicx_input_layout.h"
static settings_t settings;
static retro_keybind_set binds, original;
int main(void) {
    for(unsigned int i=0U;i<RARCH_BIND_LIST_END;++i) {
        binds[i].joykey=(uint16_t)(100U+i); binds[i].joyaxis=AXIS_POS(i);
    }
    memcpy(original,binds,sizeof(binds));settings.uints.input_analog_dpad_mode[0]=1U;
    assert(magicx_dreamcast_layout("/system/lib/libretro/gambatte_libretro.so",4,&settings,binds));
    assert(memcmp(original,binds,sizeof(binds))==0 && settings.uints.input_analog_dpad_mode[0]==1U);
    assert(!magicx_dreamcast_layout("/system/lib/libretro/flycast_libretro.so",3,&settings,binds));
    assert(memcmp(original,binds,sizeof(binds))==0);
    assert(magicx_dreamcast_layout("/system/lib/libretro/flycast_libretro.so",4,&settings,binds));
    assert(settings.uints.input_analog_dpad_mode[0]==0U);
    assert(binds[RETRO_DEVICE_ID_JOYPAD_UP].joyaxis==AXIS_NEG(1));
    assert(binds[RETRO_DEVICE_ID_JOYPAD_DOWN].joyaxis==AXIS_POS(1));
    assert(binds[RETRO_DEVICE_ID_JOYPAD_LEFT].joyaxis==AXIS_NEG(0));
    assert(binds[RETRO_DEVICE_ID_JOYPAD_RIGHT].joyaxis==AXIS_POS(0));
    assert(binds[RARCH_ANALOG_LEFT_X_PLUS].joyaxis==AXIS_POS(2));
    assert(binds[RARCH_ANALOG_LEFT_X_MINUS].joyaxis==AXIS_NEG(2));
    assert(binds[RARCH_ANALOG_LEFT_Y_PLUS].joyaxis==AXIS_POS(3));
    assert(binds[RARCH_ANALOG_LEFT_Y_MINUS].joyaxis==AXIS_NEG(3));
    assert(binds[RARCH_ANALOG_RIGHT_X_PLUS].joyaxis==AXIS_NONE);
    for(unsigned int i=0U;i<RARCH_BIND_LIST_END;++i) assert(binds[i].joykey==original[i].joykey);
    assert(binds[RARCH_ENABLE_HOTKEY].joyaxis==original[RARCH_ENABLE_HOTKEY].joyaxis);
    puts("DREAMCAST_INPUT_LAYOUT_PASS left_physical=dpad right_physical=primary_analog digital_buttons_preserved=1 other_cores_unchanged=1");
    return 0;
}

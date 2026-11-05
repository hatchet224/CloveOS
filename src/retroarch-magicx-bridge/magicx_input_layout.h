#ifndef MAGICX_INPUT_LAYOUT_H
#define MAGICX_INPUT_LAYOUT_H
/* Included after the pinned RetroArch configuration/input definitions. */
static inline bool magicx_core_basename(const char *path, const char *name) {
    const char *base = path != NULL ? strrchr(path, '/') : NULL;
    return path != NULL && strcmp(base != NULL ? base + 1 : path, name) == 0;
}
/* Use explicit physical-axis bindings for the digital Saturn pad. Generic
 * analog-to-D-pad conversion is disabled by a core's analog request; explicit
 * directions remain available across those requests and quick-menu cycles. */
static inline bool magicx_saturn_layout(const char *core, int axes,
                                        settings_t *settings, struct retro_keybind *binds) {
    if (!magicx_core_basename(core, "yabasanshiro_libretro.so")) return true;
    if (axes < 2) return false;
    binds[RETRO_DEVICE_ID_JOYPAD_UP].joyaxis = AXIS_NEG(1);
    binds[RETRO_DEVICE_ID_JOYPAD_DOWN].joyaxis = AXIS_POS(1);
    binds[RETRO_DEVICE_ID_JOYPAD_LEFT].joyaxis = AXIS_NEG(0);
    binds[RETRO_DEVICE_ID_JOYPAD_RIGHT].joyaxis = AXIS_POS(0);
    settings->uints.input_analog_dpad_mode[0] = 0U;
    settings->uints.input_libretro_device[0] = RETRO_DEVICE_JOYPAD;
    return true;
}
static inline bool magicx_dreamcast_layout(const char *core, int axes,
                                          settings_t *settings, struct retro_keybind *binds) {
    if (!magicx_core_basename(core, "flycast_libretro.so")) return true;
    if (axes < 4) return false;
    /* Physical left stick supplements the existing digital D-pad bindings.
     * Physical right stick becomes libretro's primary analog stick. */
    binds[RETRO_DEVICE_ID_JOYPAD_UP].joyaxis = AXIS_NEG(1);
    binds[RETRO_DEVICE_ID_JOYPAD_DOWN].joyaxis = AXIS_POS(1);
    binds[RETRO_DEVICE_ID_JOYPAD_LEFT].joyaxis = AXIS_NEG(0);
    binds[RETRO_DEVICE_ID_JOYPAD_RIGHT].joyaxis = AXIS_POS(0);
    for (unsigned int i = 0U; i < 4U; ++i) {
        unsigned int plus = RARCH_ANALOG_LEFT_X_PLUS + i * 2U;
        binds[plus].joyaxis = i < 2U ? AXIS_POS(i + 2U) : AXIS_NONE;
        binds[plus + 1U].joyaxis = i < 2U ? AXIS_NEG(i + 2U) : AXIS_NONE;
    }
    /* Prevent the remapped right stick from also synthesizing D-pad events. */
    settings->uints.input_analog_dpad_mode[0] = 0U;
    return true;
}
#endif

#ifndef LAUNCHER_THEME_SOUND_H
#define LAUNCHER_THEME_SOUND_H
#include <stdbool.h>
struct cl_theme_sound;
struct cl_theme_sound *cl_theme_sound_open(const char *theme, bool mute_music);
void cl_theme_sound_mute(struct cl_theme_sound *sound, bool mute);
void cl_theme_sound_click(struct cl_theme_sound *sound);
void cl_theme_sound_close(struct cl_theme_sound *sound);
#endif

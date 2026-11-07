/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_RETROARCH_H
#define MAGICX_RETROARCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool magicx_retroarch_init(void);
void magicx_retroarch_apply_language(void);
bool magicx_retroarch_poll(void); /* true: content exited; skip the ordinary runloop */
void magicx_retroarch_deinit(void);
bool magicx_retroarch_input_gated(void);
bool magicx_retroarch_core_allowed(void);
struct magicx_meter_rect { int x, y, w, h; };
bool magicx_retroarch_meter(int width, int height, int clockwise,
                           struct magicx_meter_rect out[3]);
bool magicx_retroarch_saturn(void);
bool magicx_retroarch_psp_audio(void);
bool magicx_retroarch_psx_audio(void);
int magicx_retroarch_swap_interval(int requested);
uint64_t magicx_retroarch_present_begin(void);
void magicx_retroarch_present_complete(uint64_t began);
void magicx_retroarch_frame_begin(void);
void magicx_retroarch_frame_complete(void);
bool magicx_retroarch_command_allowed(unsigned int command);
bool magicx_retroarch_requested(void);
/* Physical panel orientation is independent of managed checkpoint ownership. */
bool magicx_retroarch_portrait_menu(unsigned int width, unsigned int height);
/* Managed PSP uses linear final scaling, including downsampling a 2x image.
 * Every other core retains the configured sampling policy. */
bool magicx_retroarch_smooth_video(void);
void magicx_retroarch_hotkey_consumed(void);
/* Managed RGUI action filter: no mutation escapes into native menu callbacks. */
unsigned int magicx_retroarch_menu_action(unsigned int action, size_t selected);
void magicx_retroarch_menu_toggle(void);
bool magicx_retroarch_recovery_menu(void);
bool magicx_retroarch_playlist_menu(void);
bool magicx_retroarch_menu_finish_requested(bool quit);
bool magicx_retroarch_lifecycle_owned(void);
void magicx_retroarch_native_owned_notice(void);
const char *magicx_retroarch_menu_label(unsigned int index);
/* Exported by the pinned task_save.c integration patch, using its private
 * RASTATE serializer. These functions run only on the main thread. */
bool content_magicx_snapshot(void **data, size_t *size, size_t maximum);
bool content_magicx_legacy_save_busy(void);
#endif

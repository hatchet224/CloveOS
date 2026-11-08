/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOVE_NATIVE_SESSION_H
#define CLOVE_NATIVE_SESSION_H
#include <stdint.h>
#define CLOVE_NATIVE_MAGIC UINT32_C(0x434e5231)
#define CLOVE_NATIVE_PATH 4096U
/* Same executable ABI as the platform; records are local, never wire/public input.
 * No saved record authorizes execution. The native frontend validates load paths. */
struct clove_native_record {
 uint32_t magic, version;
 uint64_t updated;
 uint32_t slot, auto_load, auto_save, menu;
 uint32_t paused, reserved;
 char content[CLOVE_NATIVE_PATH], core[CLOVE_NATIVE_PATH];
 char config[CLOVE_NATIVE_PATH], state[CLOVE_NATIVE_PATH], save[CLOVE_NATIVE_PATH];
};
struct clove_native_import {
 uint32_t magic, version;
 uint64_t frame, size[4];
 uint32_t force, reserved;
 char directory[CLOVE_NATIVE_PATH], hash[4][65];
};
enum clove_native_operation { CLOVE_NATIVE_PAUSE=1, CLOVE_NATIVE_RESUME,
 CLOVE_NATIVE_QUIT, CLOVE_NATIVE_AUTO_SAVE };
#endif

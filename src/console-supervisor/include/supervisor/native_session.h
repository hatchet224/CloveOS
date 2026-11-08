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
#include <stdbool.h>
#include "supervisor/config.h"
bool sc_native_legacy_entry(struct cs_store *store,const struct sc_game *game,struct cl_entry *entry);
bool sc_native_read(const char *path,struct clove_native_record *record);
bool sc_native_by_key(const struct sc_config *config,const char key[65],struct clove_native_record *record);
bool sc_native_current(const struct sc_config *config,struct clove_native_record *record);
bool sc_native_game(const struct sc_config *config,const struct sc_game *game,struct clove_native_record *record);
bool sc_native_maintenance(const struct sc_config *config,const char game[65],bool delete_states);
void sc_native_states(const struct clove_native_record *record,struct cl_state_snapshot *states);
void sc_native_history(const struct sc_config *config,struct cl_history *history);
bool sc_native_request(enum clove_native_operation operation);
#endif

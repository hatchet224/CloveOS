#ifndef LAUNCHER_PREFERENCES_H
#define LAUNCHER_PREFERENCES_H
#include <stdbool.h>
#include <stdint.h>
enum cl_appearance {
    CL_HIDE_RECENT = 1U,
    CL_HIDE_ICON_LABELS = 2U,
    CL_HIDE_HINT_LABELS = 4U,
    CL_HIDE_BATTERY_PERCENTAGE = 8U,
    CL_SHOW_BATTERY_PERCENTAGE = 16U,
    CL_BATTERY_PERCENTAGE_MASK = 24U,
    CL_THEME_OVERRIDE_MASK = 30U,
    CL_DISABLE_AUTO_RESUME = 32U,
    CL_SHOW_EXPERT = 64U,
    CL_DISABLE_GAME_SERVICES = 128U,
    CL_MUTE_THEME = 256U,
    CL_GLOBAL_LOGGING = 512U,
    CL_FAVORITES_SORTED = 1024U,
    CL_POWER_TAP_SHUTDOWN = 2048U
};
/* Low two bytes retain the schema-3 MainUI X/Y assignments. */
#define CL_TRIGGER_SWAP_SHIFT 16U
#define CL_FAST_FORWARD_SHIFT 17U
#define CL_FAST_FORWARD_MAX 50U
/* Bits 24..25 retain the language in the existing checksummed schema-3 record.
 * Value 2 is Russian. Value 3 displays English without rewriting the record. */
#define CL_LANGUAGE_SHIFT 24U
#define CL_LANGUAGE_MASK (UINT32_C(3) << CL_LANGUAGE_SHIFT)
#define CL_HIDE_PORTMASTER /* Legacy migration input; Package Manager owns Ports now. */ (UINT32_C(1) << 26U)
/* Explicit acceptance makes an English first-boot choice durable too. Existing
 * valid records predate onboarding and retain their selection without a prompt. */
#define CL_LANGUAGE_CONFIRMED (UINT32_C(1) << 27U)
#define CL_SHORTCUTS_MASK UINT32_C(0xfffffff)
#define CL_APPS_SORTED (UINT64_C(1)<<16U)
#define CL_APPS_REVERSE (UINT64_C(1)<<63U)
#define CL_PREFERENCE_MASK UINT64_MAX
#define CL_MAX_MAIN_TAB 5U
#define CL_START_TAB_SHIFT 12U
#define CL_START_APP_SHIFT 15U
#define CL_START_APP_HIGH_SHIFT 23U
enum cl_start_app { CL_START_MAINUI, CL_START_SWITCHER, CL_START_RETROARCH, CL_START_ADVANCE };
static inline enum cl_start_app cl_start_application(uint64_t appearance, uint32_t shortcuts)
{
    return (enum cl_start_app)(((appearance >> CL_START_APP_SHIFT) & 1U) |
           (((shortcuts >> CL_START_APP_HIGH_SHIFT) & 1U) << 1U));
}
#define CL_BATTERY_WARN_SHIFT 17U
#define CL_BATTERY_EXIT_SHIFT 20U
#define CL_OSD_SHIFT 23U
#define CL_INGAME_MENU_SHIFT 27U
#define CL_BATTERY_FONT_SHIFT 33U
#define CL_BATTERY_SIZE_SHIFT 36U
#define CL_BATTERY_ALIGN_SHIFT 41U
#define CL_BATTERY_FIXED_SHIFT 43U
#define CL_BATTERY_X_SHIFT 45U
#define CL_BATTERY_Y_SHIFT 50U
#define CL_BRIGHTNESS_KEY_SHIFT 55U
#define CL_MAIN_MENU_SHIFT 57U
static inline uint64_t cl_setting_mask(unsigned int shift, unsigned int bits)
{ return ((UINT64_C(1) << bits) - 1U) << shift; }
static inline unsigned int cl_setting_value(uint64_t settings, unsigned int shift, unsigned int bits)
{ return (unsigned int)((settings & cl_setting_mask(shift,bits)) >> shift); }
struct cl_preference_snapshot {
    bool grid;
    unsigned int main_tab;
    uint64_t appearance, revision, request_id;
    uint32_t shortcuts;
    bool ready, pending, error, recovered, has_record;
};
struct cl_preferences;
struct cs_store;
/* Called by the supervisor on its existing qualified boot storage reader. */
bool cl_preferences_auto_resume(const struct cs_store *store);
uint64_t cl_preferences_settings(const struct cs_store *store);
uint64_t cl_preferences_runtime(const struct cs_store *store, uint32_t *shortcuts);
/* Starts an SDL storage worker. Existing .console is required; no mountpoint,
 * card directory or preferences record is created by startup. Defaults are
 * used only when both records are absent on the qualified card. */
struct cl_preferences *cl_preferences_start(const char *card_path, const char *card_uuid,
                                            bool qualified_fat_syncfs,
                                            bool default_grid, unsigned int default_main_tab);
/* Accepted writes coalesce to the newest complete pair. Success is asynchronous:
 * take() returns the last durable pair, pending/error and settled request ID. */
bool cl_preferences_submit(struct cl_preferences *worker, bool grid, unsigned int main_tab);
bool cl_preferences_submit_display(struct cl_preferences *worker, bool grid,
                                   unsigned int main_tab, uint64_t appearance);
bool cl_preferences_submit_ui(struct cl_preferences *worker, bool grid,
                              unsigned int main_tab, uint64_t appearance, uint32_t shortcuts);
bool cl_preferences_take(struct cl_preferences *worker, struct cl_preference_snapshot *snapshot);
/* Drains accepted writes and final storage barriers before joining/freeing.
 * False means persistence was not acknowledged; never authorize unmount on it. */
bool cl_preferences_stop(struct cl_preferences *worker);
/* Final quiesce only: atomically accepts the latest UI pair even while initial
 * loading is in progress, then drains/joins. Corrupt/missing media still fails
 * closed; this does not repair or overwrite a bad initial record. */
bool cl_preferences_finish_latest(struct cl_preferences *worker, bool have_latest,
                                   bool grid, unsigned int main_tab);
/* Merge only fields the user edited while the initial disk read was pending.
 * A quick tab navigation must not overwrite an unread saved list/grid choice. */
bool cl_preferences_finish_patch(struct cl_preferences *worker, bool have_grid, bool grid,
                                 bool have_tab, unsigned int main_tab);
bool cl_preferences_finish_display(struct cl_preferences *worker, bool have_grid, bool grid,
                                   bool have_tab, unsigned int main_tab,
                                   uint64_t appearance_mask, uint64_t appearance);
bool cl_preferences_finish_ui(struct cl_preferences *worker, bool have_grid, bool grid,
                              bool have_tab, unsigned int main_tab,
                              uint64_t appearance_mask, uint64_t appearance,
                              uint32_t shortcut_mask, uint32_t shortcuts);
#ifdef CL_PREFERENCES_TESTING
/* Isolated fixtures only. Production never bypasses mount/UUID qualification. */
struct cl_preferences *cl_preferences_test_start(const char *card_path,const char *card_uuid,
                                                 bool default_grid,unsigned int default_main_tab);
#endif
#endif

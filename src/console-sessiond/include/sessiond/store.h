#ifndef SESSIOND_STORE_H
#define SESSIOND_STORE_H
#include "sessiond/sha256.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#define CS_PATH_MAX 4096U
#define CS_STORE_ARTIFACTS 4U
#define CS_MANUAL_SLOTS 16U
#define CS_SCREENSHOT_MAX (4U * 1024U * 1024U)
struct cs_identity {
    char game[65], content[65], core[65], config[65], bios[65], retroarch[65], disc[65], policy[65];
};
struct cs_record {
    struct cs_identity identity;
    unsigned int schema; /* 1: legacy three artifacts; 2: coherent screenshot. */
    uint64_t generation, frame;
    uint64_t size[CS_STORE_ARTIFACTS];
    char hash[CS_STORE_ARTIFACTS][65];
    char manifest[65];
    bool menu;
};
enum cs_store_status {
    CS_STORE_OK,
    CS_STORE_EMPTY,
    CS_STORE_IO,
    CS_STORE_CORRUPT,
    CS_STORE_MEDIA,
    CS_STORE_UNSUPPORTED,
    CS_STORE_CONFLICT,
    CS_STORE_INCOMPATIBLE,
    CS_STORE_ATTEMPT_BLOCKED
};
struct cs_store_config {
    const char *card_path;
    const char *card_uuid;
    const char *instance; /* 32 lowercase random hex digits, stable for this daemon instance. */
    struct cs_identity identity;
    bool qualified_fat_syncfs;
    bool test_root; /* Unit tests only; production CLI never sets this. */
    uint64_t max_state_bytes;
};
struct cs_store {
    int card_fd, data_fd, game_fd, lock_fd;
    dev_t device;
    ino_t inode;
    uint64_t mount_id, highwater, temp_counter, staging_generation;
    char card_path[CS_PATH_MAX], uuid[65], instance[33], staging[96];
    struct cs_identity identity;
    struct cs_record selected;
    bool has_selected, fallback, fat, qualified_fat_syncfs, test_root, poisoned, read_only;
    /* UI worker only: checksummed references, manifest, type/size and PNG header
     * inspection. A writable daemon store always performs complete hashing. */
    bool presentation_only;
    uint64_t max_state_bytes;
};
bool cs_identity_valid(const struct cs_identity *identity);
bool cs_identity_equal(const struct cs_identity *a, const struct cs_identity *b);
void cs_store_init(struct cs_store *store);
void cs_store_close(struct cs_store *store);
enum cs_store_status cs_store_open(struct cs_store *store, const struct cs_store_config *config);
/* Read-only launcher view: no lock acquisition, mkdir, counter/attempt update or repair.
 * Close with cs_store_close. Use on a storage worker, never on the SDL event thread. */
enum cs_store_status cs_store_open_reader(struct cs_store *store,
                                          const struct cs_store_config *config);
enum cs_store_status cs_store_read_checkpoint(struct cs_store *store,
                                              const struct cs_identity *identity,
                                              uint64_t generation, const char manifest_hash[65],
                                              struct cs_record *record);
enum cs_store_status cs_store_read_resume(struct cs_store *store, bool previous,
                                          struct cs_record *record);
enum cs_store_status cs_store_check_media(const struct cs_store *store);
/* Final checked volume barrier, used only after actual writer completion. */
enum cs_store_status cs_store_sync(struct cs_store *store);
enum cs_store_status cs_store_load(struct cs_store *store);
enum cs_store_status cs_store_stage(struct cs_store *store, uint64_t generation,
                                    char relative[256]);
enum cs_store_status cs_store_commit(struct cs_store *store, uint64_t frame);
enum cs_store_status cs_store_menu(struct cs_store *store);
enum cs_store_status cs_store_verify_selected(struct cs_store *store, uint64_t generation,
                                              char relative[256]);
enum cs_store_status cs_store_attempt(struct cs_store *store, bool completed);
enum cs_store_status cs_store_slot_save(struct cs_store *store, unsigned int slot);
enum cs_store_status cs_store_slot_read(struct cs_store *store, const struct cs_identity *identity,
                                        unsigned int slot, struct cs_record *record);
enum cs_store_status cs_store_export_screenshot(struct cs_store *store);
/* Idle daemon only, holding the card writer lock. Remove checkpoint/state
 * artifacts for this exact game hash; keep every SRAM/RTC artifact. */
enum cs_store_status cs_store_delete_states(struct cs_store *store, const char game[65]);
const char *cs_store_status_name(enum cs_store_status status);
#endif

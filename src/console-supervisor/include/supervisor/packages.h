#ifndef SUPERVISOR_PACKAGES_H
#define SUPERVISOR_PACKAGES_H
#include "sessiond/store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SP_PACKAGE_COUNT 35U
#define SP_PACKAGE_PATH 160U
/* Stable bit positions: never reorder existing IDs in a compatible schema. */
enum sp_package_layer { SP_PACKAGE_RUNTIME, SP_PACKAGE_VERIFIED, SP_PACKAGE_APPS, SP_PACKAGE_EXPERT };
static inline bool sp_package_core_layer(enum sp_package_layer layer)
{
    return layer == SP_PACKAGE_VERIFIED || layer == SP_PACKAGE_EXPERT;
}
enum sp_package_status {
    SP_PACKAGE_OK, SP_PACKAGE_EMPTY, SP_PACKAGE_RECOVERED, SP_PACKAGE_CORRUPT,
    SP_PACKAGE_INCOMPATIBLE, SP_PACKAGE_MEDIA, SP_PACKAGE_IO,
    SP_PACKAGE_CONFLICT, SP_PACKAGE_UNAVAILABLE, SP_PACKAGE_REQUIRED,
    SP_PACKAGE_UNCERTAIN
};
struct sp_package {
    const char *id, *title, *systems, *artifact;
    enum sp_package_layer layer;
    uint64_t dependencies;
    bool required, default_enabled;
};
struct sp_package_registry {
    char fingerprint[65];
    char artifact_sha256[SP_PACKAGE_COUNT][65];
    uint64_t available;
};
struct sp_package_selection {
    uint64_t revision, enabled;
    bool recovered, initialized;
};
struct sp_package_plan {
    uint64_t revision, original, desired;
    unsigned int installs, removals;
};
const struct sp_package *sp_package_at(size_t index);
int sp_package_index(const char *id);
/* Protected manifest relative to a trusted root fd; verifies exact fixed native
 * artifact paths, root ownership/modes, ELF64 AArch64 and SHA256. No card paths
 * or executable commands become part of the registry. Runs on storage worker. */
bool sp_package_registry_load(int trusted_root_fd, const char *manifest_relative,
                              struct sp_package_registry *registry);
/* Fast menu inventory from protected manifest/ELF headers. This is not content
 * authentication: launch preflight still hashes the selected ROM/core/emulator. */
bool sp_package_registry_describe(int trusted_root_fd, const char *manifest_relative,
                                  struct sp_package_registry *registry);
uint64_t sp_package_defaults(const struct sp_package_registry *registry);
bool sp_package_selection_valid(const struct sp_package_registry *registry, uint64_t enabled);
void sp_package_plan_begin(struct sp_package_plan *plan, const struct sp_package_selection *selection);
enum sp_package_status sp_package_plan_set(const struct sp_package_registry *registry,
                                            struct sp_package_plan *plan, size_t index, bool enabled);
bool sp_package_enabled(const struct sp_package_selection *selection, const char *id);
bool sp_package_system_enabled(const struct sp_package_selection *selection, const char *folder);
/* .console/packages.json and packages.previous.json on the already-qualified
 * volume. Caller owns volume lifetime and runs I/O on its storage worker.
 * CAS revision prevents lost updates. No startup scripts or code are installed.
 * RECOVERED selects last valid previous record; caller must display recovery.
 * UNCERTAIN means rename occurred but barrier failed: reload before retry.
 * Tests may inject faults only when compiled with SP_PACKAGE_TESTING. */
enum sp_package_status sp_package_load(const struct cs_store *volume,
                                       const struct sp_package_registry *registry,
                                       struct sp_package_selection *selection);
enum sp_package_status sp_package_apply(const struct cs_store *volume,
                                        const struct sp_package_registry *registry,
                                        const struct sp_package_plan *plan,
                                        struct sp_package_selection *selection);
enum sp_package_status sp_package_rollback(const struct cs_store *volume,
                                           const struct sp_package_registry *registry,
                                           uint64_t expected_revision,
                                           struct sp_package_selection *selection);
const char *sp_package_status_name(enum sp_package_status status);
#ifdef SP_PACKAGE_TESTING
/* Test-only boundary fault: 1 before previous write, 2 before current rename,
 * 3 after current rename/before directory barrier. Never compiled into target. */
void sp_package_test_fault(unsigned int stage);
#endif
#endif

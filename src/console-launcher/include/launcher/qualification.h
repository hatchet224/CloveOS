#ifndef LAUNCHER_QUALIFICATION_H
#define LAUNCHER_QUALIFICATION_H
#include "launcher/model.h"
#define CL_INPUT_RECORD_MAX 2048U
struct cl_input_verification {
    struct cl_input_profile baseline, checked;
};
/* Navigation-8 verification permits only the wizard's paired A/B+X/Y label swap.
 * Other controls remain the measured baseline; this is not global-hotkey proof.
 * Schema2 appends measured START/SELECT. A schema1 checksum is verified using
 * its original13 controls before merging only current measured new controls. */
bool cl_input_verification_valid(const struct cl_input_profile *current,
                                 const struct cl_input_verification *verification);
bool cl_input_record_encode(const char *uuid, const struct cl_input_verification *verification,
                            char output[CL_INPUT_RECORD_MAX], size_t *bytes);
bool cl_input_record_decode(const char *input, size_t bytes, const char *uuid,
                            const struct cl_input_profile *current,
                            struct cl_input_verification *verification);
/* Worker-only settings I/O through an already UUID/mount-verified volume pin.
 * Never touches checkpoint pointers or the daemon's writer lock. */
enum cs_store_status cl_input_record_load(struct cs_store *store,
                                          const struct cl_input_profile *current,
                                          struct cl_input_verification *verification);
enum cs_store_status cl_input_record_save(struct cs_store *store,
                                          const struct cl_input_verification *verification);
/* Protected runtime handoff for input-only RetroArch overrides. */
bool cl_input_runtime_publish(const struct cl_input_verification *verification);
#endif

#ifndef SUPERVISOR_ONLINE_H
#define SUPERVISOR_ONLINE_H
#include "supervisor/config.h"
#include "supervisor/transfer.h"
enum sc_online_field { SC_ONLINE_USER = 1, SC_ONLINE_PASSWORD, SC_ONLINE_DEVID,
    SC_ONLINE_DEVPASSWORD, SC_ONLINE_REPOSITORY, SC_ONLINE_REGION, SC_ONLINE_MEDIA };
struct sc_online_settings {
    char user[64], password[64], devid[64], devpassword[64], repository[64];
    unsigned int region, media;
};
/* All service calls execute on the cancellable supervisor storage worker. */
void sc_online_request(const struct sc_config *config, const struct cs_store *store,
                         const struct cl_tool_request *request, struct cl_tool_snapshot *snapshot,
                         const struct sc_transfer_cancel *cancel);
bool sc_online_settings_load(const struct cs_store *store, struct sc_online_settings *settings);
bool sc_online_settings_save(const struct cs_store *store, const struct sc_online_settings *settings);
bool sc_online_normalize_image(const unsigned char *bytes, size_t size,
                                 unsigned char **png, size_t *png_size);
/* Startup only: qualified cached files may repair their exact protected core
 * placeholders, through immutable tmpfs copies. No executable runs from FAT. */
bool sc_online_restore_packages(const struct sc_config *config);
#endif

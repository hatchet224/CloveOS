#ifndef SESSIOND_PROFILE_H
#define SESSIOND_PROFILE_H
#include "sessiond/store.h"
struct cs_profile {
    struct cs_identity identity;
    char card_path[512], card_uuid[65], bridge[108], input_name[256], input_path[256];
    unsigned int key_code;
    uid_t bridge_uid;
    uint64_t max_state_bytes;
    bool qualified_fat_syncfs, attach_running;
};
bool cs_profile_load(const char *path, struct cs_profile *profile);
#endif

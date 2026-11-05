#ifndef LAUNCHER_FILE_EXPLORER_H
#define LAUNCHER_FILE_EXPLORER_H
#include "launcher/model.h"
#define CL_FILE_EXPLORER_PATH "/system/bin/console-file-explorer"
/* Sent by the supervisor over the private socket inherited at fd 3. */
struct cl_file_explorer_config {
    struct cl_display_config display;
    struct cl_input_profile input;
    char card_uuid[65];
    bool qualified_fat_syncfs;
};
#endif

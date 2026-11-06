/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LAUNCHER_PORTS_H
#define LAUNCHER_PORTS_H
#include "launcher/file_explorer.h"
struct cl_ports_config {
    struct cl_file_explorer_config device;
    unsigned int language;
};
#endif

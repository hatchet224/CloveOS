#ifndef SUPERVISOR_TOOLS_BACKEND_H
#define SUPERVISOR_TOOLS_BACKEND_H
#include "supervisor/config.h"
#include "supervisor/packages.h"
unsigned int sc_tools_app_features(const struct sp_package_selection *selection);
#include "supervisor/transfer.h"
struct sc_tools_backend
{
    struct sp_package_registry registry;
    bool registry_loaded;
    struct sc_transfer_cancel cancel;
    char files_directory[512];
};
/* Worker-thread only; the registry contains only protected native artifacts. */
bool sc_tools_packages(struct sc_tools_backend *backend, const struct cs_store *store,
                       struct sp_package_selection *selection);
bool sc_tools_system_visible(const struct sp_package_selection *selection, const char *system);
bool sc_tools_system_expert(const char *system);
bool sc_tools_game_enabled(struct sc_tools_backend *backend, const struct cs_store *store,
                           const struct sc_game *game);
void sc_tools_request(struct sc_tools_backend *backend, const struct sc_config *config,
                      const struct cs_store *store, const struct cl_tool_request *request,
                      struct cl_tool_snapshot *snapshot);
bool sc_tools_selected_icons(const struct sc_config *c, const struct cs_store *store,
                             char selected[512], char icon_theme[512]);
bool sc_tools_selected_theme(const struct sc_config *config, const struct cs_store *store,
                             char selected[512]);
#endif

#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum cl_theme_role {
    CL_FONT_LIST, CL_FONT_TITLE, CL_FONT_HINT, CL_FONT_DETAIL,
    CL_FONT_GRID_MAIN, CL_FONT_GRID_SYSTEM, CL_FONT_COUNT
};
struct cl_font_style {
    char font[128];
    int size;
    uint32_t color;
};
struct cl_theme {
    char name[65];
    struct cl_font_style fonts[CL_FONT_COUNT];
    uint32_t background, panel, selected, selected_text, accent, muted, ready, warning;
    int border_left, border_right, list_top, row_height, row_gap, radius;
    int grid_columns, grid_rows, grid_gap, grid1x4, grid3x4;
    bool grid, show_battery, hide_hints, hide_icon_labels;
};
void cl_theme_default(struct cl_theme *theme);
/* Onion theme properties + a documented Moss layout/palette extension. Overlay
 * parses atomically; bad geometry/type/color leaves the previous theme intact. */
bool cl_theme_apply(struct cl_theme *theme, const char *json, size_t bytes);
bool cl_theme_valid(const struct cl_theme *theme);
/* Startup only; component-wise no-symlink opens, bounded JSON, no writes. */
bool cl_theme_read(struct cl_theme *theme, const char *path);
#endif

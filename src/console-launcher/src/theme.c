#include "launcher/theme.h"
#include "sessiond/document.h"
#include <stdlib.h>
#include <string.h>

void cl_theme_default(struct cl_theme *t)
{
    size_t i;
    memset(t, 0, sizeof(*t));
    strcpy(t->name, "Clove");
    for (i = 0U; i < CL_FONT_COUNT; ++i) {
        strcpy(t->fonts[i].font, i == CL_FONT_LIST ?
               "/mnt/SDCARD/miyoo/app/wqy-microhei.ttc" :
               "/mnt/SDCARD/miyoo/app/Exo-2-Bold-Italic_Universal.ttf");
        t->fonts[i].size =
            i == CL_FONT_TITLE ? 32 : (i == CL_FONT_HINT || i == CL_FONT_DETAIL ? 16 : 24);
        t->fonts[i].color = 0x202124U;
    }
    t->background = 0xf4f2eeU;
    t->panel = 0xe9e5dfU;
    t->selected = 0x69548fU;
    t->selected_text = 0xffffffU;
    t->accent = 0x69548fU;
    t->muted = 0x68646eU;
    t->ready = 0x277751U;
    t->warning = 0x9d5218U;
    t->border_left = 16;
    t->border_right = 16;
    t->list_top = 80;
    t->row_height = 52;
    t->row_gap = 4;
    t->radius = 12;
    t->grid_columns = 3;
    t->grid_rows = 2;
    t->grid_gap = 12;
    t->grid1x4 = 24;
    t->grid3x4 = 18;
    t->fonts[CL_FONT_GRID_MAIN].size = t->grid1x4;
    t->fonts[CL_FONT_GRID_SYSTEM].size = t->grid3x4;
    t->show_battery = true;
}
bool cl_theme_valid(const struct cl_theme *t)
{
    size_t i;
    if (t->border_left < 0 || t->border_left > 48 || t->border_right < 0 || t->border_right > 48 ||
        t->list_top < 60 || t->list_top > 96 || t->row_height < 44 || t->row_height > 60 ||
        t->list_top + 6 * t->row_height > 420 || t->row_gap < 0 || t->row_gap > 8 ||
        t->radius < 0 || t->radius > 18 || t->grid_columns < 1 || t->grid_columns > 3 ||
        t->grid_rows < 2 || t->grid_rows > 6 || t->grid_rows * t->grid_columns != 6 ||
        t->grid_gap < 4 || t->grid_gap > 24 || t->grid1x4 < 12 || t->grid1x4 > 40 ||
        t->grid3x4 < 12 || t->grid3x4 > 40) {
        return false;
    }
    for (i = 0U; i < CL_FONT_COUNT; ++i) {
        if (t->fonts[i].size < 12 || t->fonts[i].size > 40 ||
            (t->fonts[i].font[0] == '/' &&
             strncmp(t->fonts[i].font, "/mnt/SDCARD/miyoo/app/", 22U) != 0) ||
            strstr(t->fonts[i].font, "..") != NULL || strchr(t->fonts[i].font, '\\') != NULL) {
            return false;
        }
    }
    return t->fonts[CL_FONT_LIST].size <= 28 && t->fonts[CL_FONT_HINT].size <= 28 &&
           t->fonts[CL_FONT_DETAIL].size <= 20;
}
static bool color(const struct cs_document *d, size_t parent, const char *name, uint32_t *out)
{
    char text[48];
    size_t t = cs_doc_get(d, parent, name), i;
    uint32_t value = 0U;
    if (t == SIZE_MAX) {
        return true;
    }
    if (!cs_doc_text(d, t, text, sizeof(text))) {
        return false;
    }
    if (strncmp(text, "rgb(", 4U) == 0) {
        const char *p = text + 4U;
        for (i = 0U; i < 3U; ++i) {
            unsigned int component = 0U, digits = 0U;
            while (*p == ' ' || *p == '\t') ++p;
            while (*p >= '0' && *p <= '9') {
                component = component * 10U + (unsigned int)(*p++ - '0');
                if (++digits > 3U || component > 255U) return false;
            }
            while (*p == ' ' || *p == '\t') ++p;
            if (digits == 0U || *p++ != (i == 2U ? ')' : ',')) return false;
            value = (value << 8U) | component;
        }
        if (*p != '\0') return false;
        *out = value;
        return true;
    }
    if (strlen(text) != 6U && !(strlen(text) == 7U && text[0] == '#')) return false;
    for (i = 0U; i < 6U; ++i) {
        unsigned int digit;
        char c = text[i + (text[0] == '#' ? 1U : 0U)];
        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned int)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned int)(c - 'A') + 10U;
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    *out = value;
    return true;
}
static bool number(const struct cs_document *d, size_t parent, const char *name, int *out)
{
    size_t t = cs_doc_get(d, parent, name);
    return t == SIZE_MAX || cs_doc_int(d, t, out);
}
static bool boolean(const struct cs_document *d, size_t parent, const char *name, bool *out)
{
    size_t t = cs_doc_get(d, parent, name);
    return t == SIZE_MAX || cs_doc_bool(d, t, out);
}
static bool object(const struct cs_document *d, const char *name, size_t *out)
{
    *out = cs_doc_get(d, 0U, name);
    return *out == SIZE_MAX || d->tokens[*out].type == CS_DOC_OBJECT;
}
bool cl_theme_apply(struct cl_theme *theme, const char *json, size_t bytes)
{
    struct cs_document *d = calloc(1U, sizeof(*d));
    struct cl_theme t = *theme;
    size_t node, i;
    bool ok = false;
    static const char *const roles[] = {"list", "title", "hint", "detail"};
    if (d == NULL || !cs_doc_parse(d, json, bytes) || d->tokens[0].type != CS_DOC_OBJECT) {
        goto done;
    }
    node = cs_doc_get(d, 0U, "name");
    if (node != SIZE_MAX && !cs_doc_text(d, node, t.name, sizeof(t.name))) {
        goto done;
    }
    for (i = 0U; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        size_t font;
        if (!object(d, roles[i], &node)) {
            goto done;
        }
        font = cs_doc_get(d, node, "font");
        if (font != SIZE_MAX && !cs_doc_text(d, font, t.fonts[i].font, sizeof(t.fonts[i].font))) {
            goto done;
        }
        if (!number(d, node, "size", &t.fonts[i].size) ||
            !color(d, node, "color", &t.fonts[i].color)) {
            goto done;
        }
    }
    if (!object(d, "frame", &node) || !number(d, node, "border-left", &t.border_left) ||
        !number(d, node, "border-right", &t.border_right)) {
        goto done;
    }
    if (!object(d, "grid", &node) || !number(d, node, "grid1x4", &t.grid1x4) ||
        !number(d, node, "grid3x4", &t.grid3x4) ||
        !color(d, node, "selectedcolor", &t.selected_text)) {
        goto done;
    }
    {
        size_t font = cs_doc_get(d, node, "font");
        if (font != SIZE_MAX &&
            !cs_doc_text(d, font, t.fonts[CL_FONT_GRID_MAIN].font,
                          sizeof(t.fonts[CL_FONT_GRID_MAIN].font))) goto done;
        if (!color(d, node, "color", &t.fonts[CL_FONT_GRID_MAIN].color)) goto done;
        t.fonts[CL_FONT_GRID_MAIN].size = t.grid1x4;
        t.fonts[CL_FONT_GRID_SYSTEM] = t.fonts[CL_FONT_GRID_MAIN];
        t.fonts[CL_FONT_GRID_SYSTEM].size = t.grid3x4;
    }
    if (!object(d, "batteryPercentage", &node) || !boolean(d, node, "visible", &t.show_battery)) {
        goto done;
    }
    if (!object(d, "hideLabels", &node) || !boolean(d, node, "hints", &t.hide_hints) ||
        !boolean(d, node, "icons", &t.hide_icon_labels)) {
        goto done;
    }
    /* Stock themes use a zero- or one-point hint font to suppress labels. */
    if (t.fonts[CL_FONT_HINT].size == 0 || t.fonts[CL_FONT_HINT].size == 1) {
        t.hide_hints = true;
        t.fonts[CL_FONT_HINT].size = 16;
    }
    if (!object(d, "palette", &node) || !color(d, node, "background", &t.background) ||
        !color(d, node, "panel", &t.panel) || !color(d, node, "selected", &t.selected) ||
        !color(d, node, "selectedText", &t.selected_text) || !color(d, node, "accent", &t.accent) ||
        !color(d, node, "muted", &t.muted) || !color(d, node, "ready", &t.ready) ||
        !color(d, node, "warning", &t.warning)) {
        goto done;
    }
    if (!object(d, "layout", &node) || !number(d, node, "listTop", &t.list_top) ||
        !number(d, node, "rowHeight", &t.row_height) || !number(d, node, "rowGap", &t.row_gap) ||
        !number(d, node, "radius", &t.radius) || !number(d, node, "gridColumns", &t.grid_columns) ||
        !number(d, node, "gridRows", &t.grid_rows) || !number(d, node, "gridGap", &t.grid_gap)) {
        goto done;
    }
    {
        size_t mode = cs_doc_get(d, node, "mode");
        char name[8];
        if (mode != SIZE_MAX) {
            if (!cs_doc_text(d, mode, name, sizeof(name)) ||
                (strcmp(name, "list") != 0 && strcmp(name, "grid") != 0)) {
                goto done;
            }
            t.grid = strcmp(name, "grid") == 0;
        }
    }
    if (strcmp(t.name, "Silky") == 0) {
        if (cs_doc_get(d, 0U, "palette") == SIZE_MAX) {
            t.background = 0x10121cU;
            t.panel = 0x252733U;
            t.selected = 0x7045bdU;
            t.selected_text = 0xffffffU;
            t.accent = 0x9363d9U;
            t.muted = 0xbab9c7U;
        }
    }
    if (cs_doc_get(d, 0U, "detail") == SIZE_MAX) {
        t.fonts[CL_FONT_DETAIL] = t.fonts[CL_FONT_HINT];
        t.fonts[CL_FONT_DETAIL].size = 16;
    }
    if (!cl_theme_valid(&t)) {
        goto done;
    }
    *theme = t;
    ok = true;
done:
    free(d);
    return ok;
}

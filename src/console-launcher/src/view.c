#include "launcher/preferences.h"
#include "launcher/i18n.h"
#include "launcher/view.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static Uint32 rgb(SDL_Surface *s, uint32_t c)
{
    return SDL_MapRGB(s->format, (Uint8)(c >> 16U), (Uint8)(c >> 8U), (Uint8)c);
}
static int brightness(uint32_t color)
{
    return (int)(((color >> 16U) & 255U) * 299U + ((color >> 8U) & 255U) * 587U +
                 (color & 255U) * 114U) / 1000;
}

static uint32_t readable_ink(uint32_t ink, uint32_t background)
{
    int level = brightness(background);
    int difference = brightness(ink) - level;
    if (difference > 95 || difference < -95)
        return ink;
    return level >= 128 ? 0x111111U : 0xf5f5f5U;
}
void cl_view_text(SDL_Surface *s, struct cl_assets *a, enum cl_theme_role role, int x, int y,
                  const char *text, uint32_t color, int width)
{
    if (cl_runtime_text(a, role, s, x, y, text, color, width)) {
        return;
    }
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; ++p)
        if (*p >= 128U) { ascii = false; break; }
    if (ascii && a->fonts[role].surface != NULL) {
        cl_atlas_text(&a->fonts[role], s, x, y, text, color, 120U, width);
    } else {
        unsigned int scale=cl_compact_text_scale(a,role);
        cl_text(s, x, y, text, scale, rgb(s, color), (size_t)width/(6U*scale));
    }
}
static void fit(SDL_Surface *target, SDL_Surface *image, SDL_Rect bounds)
{
    SDL_Rect dst = bounds;
    if (image == NULL) {
        return;
    }
    if ((int64_t)image->w * bounds.h > (int64_t)image->h * bounds.w) {
        dst.h = (int)((int64_t)image->h * bounds.w / image->w);
    } else {
        dst.w = (int)((int64_t)image->w * bounds.h / image->h);
    }
    dst.x += (bounds.w - dst.w) / 2;
    dst.y += (bounds.h - dst.h) / 2;
    (void)SDL_BlitScaled(image, NULL, target, &dst);
}
static void image_rect(SDL_Surface *target, SDL_Surface *image, SDL_Rect rect)
{
    if (image != NULL) {
        (void)SDL_BlitScaled(image, NULL, target, &rect);
    }
}
static void fit_label(SDL_Surface *s, const struct cl_view *v, enum cl_theme_role role,
                       SDL_Rect bounds, const char *text, uint32_t ink)
{
    int width, height;
    SDL_Surface *label;
    if (!cl_text_measure(v->assets, role, text, &width, &height) || width < 1 || height < 1) return;
    if (width <= bounds.w && height <= bounds.h) {
        cl_view_text(s, v->assets, role, bounds.x, bounds.y + (bounds.h-height)/2, text, ink, bounds.w);
        return;
    }
    label = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (label == NULL) return;
    (void)SDL_FillRect(label, NULL, 0U);
    cl_view_text(label, v->assets, role, 0, 0, text, ink, width);
    (void)SDL_SetSurfaceBlendMode(label, SDL_BLENDMODE_BLEND);
    fit(s, label, bounds);
    SDL_FreeSurface(label);
}
void cl_view_frame(SDL_Surface *s, const struct cl_view *v, const char *title)
{
    const struct cl_theme *t = v->theme;
    bool show_battery = (v->appearance & CL_SHOW_BATTERY_PERCENTAGE) != 0U ||
        (t->show_battery && (v->appearance & CL_HIDE_BATTERY_PERCENTAGE) == 0U);
    char status[32], clock[8] = "--:--";
    int clock_width=0,clock_height=0;
    time_t raw = time(NULL);
    struct tm now;
    (void)SDL_FillRect(s, NULL, rgb(s, t->background));
    image_rect(s, v->assets->background, (SDL_Rect){0, 0, 640, 480});
    image_rect(s, v->assets->title, (SDL_Rect){0, 0, 640, 60});
    image_rect(s, v->assets->footer, (SDL_Rect){0, 420, 640, 60});
    if (raw > (time_t)1577836800) {
#ifdef _WIN32
        if (localtime_s(&now, &raw) == 0) {
            (void)strftime(clock, sizeof(clock), "%H:%M", &now);
        }
#else
        tzset();
        if (localtime_r(&raw, &now) != NULL) {
            (void)strftime(clock, sizeof(clock), "%H:%M", &now);
        }
#endif
    }
    {
        int right=show_battery?476:548;
        bool clock_fits=cl_text_measure(v->assets,CL_FONT_DETAIL,clock,&clock_width,&clock_height) &&
                        clock_width>0 && clock_width<=right-24;
        int title_width=clock_fits?right-clock_width-36:380;
        if(title_width>380) title_width=380;
        if(title_width>0)
            fit_label(s,v,CL_FONT_TITLE,(SDL_Rect){24,6,title_width,48},title,
                      t->fonts[CL_FONT_TITLE].color);
        if(clock_fits)
            cl_view_text(s,v->assets,CL_FONT_DETAIL,right-clock_width,
                         clock_height<60?(60-clock_height)/2:0,clock,
                         t->fonts[CL_FONT_TITLE].color,clock_width);
    }
    {
        size_t level =
            v->battery < 1
                ? 0U
                : (v->battery < 25 ? 1U : (v->battery < 55 ? 2U : (v->battery < 85 ? 3U : 4U)));
        SDL_Surface *icon = v->external_power ? v->assets->charging : v->assets->battery[level];
        image_rect(s, icon, (SDL_Rect){544, 6, 88, 48});
        unsigned int threshold=cl_setting_value(v->appearance,CL_BATTERY_WARN_SHIFT,3U)*5U;
        bool low=!v->external_power && threshold!=0U && v->battery>=0 &&
                 v->battery<=(int)threshold;
        if (low) cl_round_rect(s,(SDL_Rect){621,12,10,10},4,0xf44a50U);
        if (v->battery < 0) {
            (void)snprintf(status, sizeof(status), "--%%");
        } else {
            (void)snprintf(status, sizeof(status), "%d%%", v->battery);
        }
        if (show_battery) {
            static const enum cl_theme_role roles[] = {CL_FONT_DETAIL,CL_FONT_LIST,
                                                       CL_FONT_TITLE,CL_FONT_HINT,CL_FONT_GRID_MAIN};
            unsigned int family=cl_setting_value(v->appearance,CL_BATTERY_FONT_SHIFT,3U);
            unsigned int size=cl_setting_value(v->appearance,CL_BATTERY_SIZE_SHIFT,5U);
            unsigned int align=cl_setting_value(v->appearance,CL_BATTERY_ALIGN_SHIFT,2U);
            unsigned int fixed=cl_setting_value(v->appearance,CL_BATTERY_FIXED_SHIFT,2U);
            unsigned int x_shift=cl_setting_value(v->appearance,CL_BATTERY_X_SHIFT,5U);
            unsigned int y_shift=cl_setting_value(v->appearance,CL_BATTERY_Y_SHIFT,5U);
            enum cl_theme_role role=roles[family<sizeof(roles)/sizeof(roles[0])?family:0U];
            int w=0,h=0;
            (void)cl_text_measure(v->assets,role,status,&w,&h);
            if (size!=0U && t->fonts[role].size>0)
                w=w*(int)(size+11U)/t->fonts[role].size;
            if (w<1) w=52;
            int x=fixed==2U?482+(clock_width%12):490;
            if (align==2U) x+=(52-w)/2;
            else if (align==3U) x+=52-w;
            int y=22;
            if (x_shift!=0U) x+=(int)x_shift-16;
            if (y_shift!=0U) y+=(int)y_shift-16;
            uint32_t ink=low?0xf44a50U:t->fonts[CL_FONT_TITLE].color;
            if (size==0U) cl_view_text(s,v->assets,role,x,y,status,ink,80);
            else fit_label(s,v,role,(SDL_Rect){x,y,w,(int)(size+11U)+7},status,ink);
        }
    }
}
void cl_view_footer(SDL_Surface *s, const struct cl_view *v, const char *hints)
{
    hints=CL_TR(hints);
    SDL_Rect footer = {0, 420, 640, 60};
    (void)SDL_FillRect(s, &footer, rgb(s, v->theme->background));
    image_rect(s, v->assets->footer, footer);
    if (v->theme->hide_hints || (v->appearance & CL_HIDE_HINT_LABELS) != 0U)
        return;
    /* Long translated hints must retain every action, including Back. */
    int hint_width=0,hint_height=0;
    if (cl_text_measure(v->assets,CL_FONT_HINT,hints,&hint_width,&hint_height) && hint_width>500) {
        fit_label(s,v,CL_FONT_HINT,(SDL_Rect){24,432,592,36},hints,
                  v->theme->fonts[CL_FONT_HINT].color);
        return;
    }
    int x = 24;
    const char *part = hints;
    while (*part != '\0' && x < 616) {
        char label[128];
        const char *end = strstr(part, "  ");
        size_t length = end != NULL ? (size_t)(end-part) : strlen(part);
        if (length >= sizeof(label)) length = sizeof(label)-1U;
        memcpy(label, part, length); label[length] = '\0';
        SDL_Surface *icon = label[0] == 'A' && label[1] == ' ' ? v->assets->hint_a :
                            (label[0] == 'B' && label[1] == ' ' ? v->assets->hint_b : NULL);
        const char *caption = icon != NULL ? label+2 : label;
        int width = 0, height = 0;
        if (icon != NULL) { image_rect(s, icon, (SDL_Rect){x, 434, 32, 32}); x += 40; }
        if (cl_text_measure(v->assets, CL_FONT_HINT, caption, &width, &height)) {
            cl_view_text(s, v->assets, CL_FONT_HINT, x, 450-height/2, caption,
                         v->theme->fonts[CL_FONT_HINT].color, 616-x);
            x += width+24;
        }
        if (end == NULL) break;
        part = end; while (*part == ' ') ++part;
    }
}
void cl_main_draw(SDL_Surface *s, const struct cl_view *v, unsigned int tab)
{
    static const char *const labels[] = {"Favorites", "Games", "Apps", "Settings", "Recent", "Expert"};
    unsigned int order[6];
    size_t count=0U;
    if ((v->appearance&CL_HIDE_RECENT)==0U) order[count++]=4U;
    order[count++]=0U;order[count++]=1U;
    if ((v->appearance&CL_SHOW_EXPERT)!=0U) order[count++]=5U;
    order[count++]=2U;order[count++]=3U;
    size_t selection = 0U, first;
    bool found=false;
    for (size_t i=0U;i<count;++i) if(order[i]==tab){selection=i;found=true;}
    if(!found){tab=1U;for(size_t i=0U;i<count;++i)if(order[i]==tab)selection=i;}
    first=selection>3U?selection-3U:0U;
    cl_view_frame(s, v, "CloveOS");
    for (size_t i = 0U; i < 4U; ++i) {
        unsigned int item = order[first+i];
        SDL_Rect rect = {4 + (int)i * 158, 78, 156, 328};
        bool active = item == tab;
        if (item<5U && v->assets->tabs[item][active ? 1U : 0U] != NULL)
            image_rect(s, v->assets->tabs[item][active ? 1U : 0U], rect);
        else cl_round_rect(s, rect, v->theme->radius, active ? v->theme->selected : v->theme->panel);
        if (item==5U)
            fit_label(s,v,CL_FONT_TITLE,(SDL_Rect){rect.x+10,168,136,65},CL_TR("Expert"),
                      readable_ink(v->theme->selected_text,active?v->theme->selected:v->theme->panel));
        else if (!v->theme->hide_icon_labels && (v->appearance & CL_HIDE_ICON_LABELS) == 0U)
            fit_label(s, v, CL_FONT_GRID_MAIN, (SDL_Rect){rect.x+4, 332, 148, 44}, CL_TR(labels[item]),
                      active ? v->theme->selected_text : v->theme->fonts[CL_FONT_GRID_MAIN].color);
    }
    cl_view_footer(s, v, CL_TR("A Select                       Left/Right"));
}
void cl_level_meter(SDL_Surface *s, const struct cl_view *v, bool brightness_level)
{
    unsigned int setting=cl_setting_value(v->appearance,CL_OSD_SHIFT,4U);
    unsigned int level=brightness_level?v->brightness_level:v->volume_level;
    unsigned int maximum=brightness_level?11U:20U;
    int width=setting==0U?28:12+(int)setting*3;
    if (level>maximum) level=maximum;
    int filled=(int)(level*170U/maximum);
    cl_round_rect(s,(SDL_Rect){12,132,width+12,194},9,v->theme->panel);
    cl_round_rect(s,(SDL_Rect){18,144,width,170},5,v->theme->background);
    if(filled>0) cl_round_rect(s,(SDL_Rect){18,314-filled,width,filled},5,v->theme->accent);
}
void cl_view_dialog(SDL_Surface *s, const struct cl_view *v, const char *title,
                    const char *message, const char *cancel, const char *confirm, size_t selected)
{
    title=CL_TR(title);message=CL_TR(message);cancel=CL_TR(cancel);confirm=CL_TR(confirm);
    cl_view_frame(s, v, title);
    if (v->assets->popup != NULL)
        image_rect(s, v->assets->popup, (SDL_Rect){0, 80, 640, 330});
    else cl_round_rect(s, (SDL_Rect){16, 90, 608, 308}, 12, v->theme->panel);
    size_t used = 0U, length = strnlen(message, 512U);
    for (int y = 118; used < length && y < 286; y += 36) {
        char line[512]; size_t take = 0U, space = 0U;
        int width = 0, height = 0;
        while (used+take < length && take < sizeof(line)-1U) {
            line[take] = message[used+take]; ++take; line[take] = '\0';
            if (cl_text_measure(v->assets, CL_FONT_LIST, line, &width, &height) && width > 548) { --take; break; }
            if (line[take-1U] == ' ') space = take-1U;
        }
        if (used+take < length && space > 0U) take = space;
        if (take == 0U) take = 1U;
        /* Never split a UTF-8 code point at the width boundary. */
        while (take > 1U && ((unsigned char)message[used+take] & 0xc0U) == 0x80U) --take;
        memcpy(line, message+used, take); line[take] = '\0';
        cl_view_text(s, v->assets, CL_FONT_LIST, 46, y, line, v->theme->fonts[CL_FONT_LIST].color, 548);
        used += take; while (used < length && message[used] == ' ') ++used;
    }
    for (size_t i = cancel != NULL ? 0U : 1U; i < 2U; ++i) {
        SDL_Rect button = {cancel != NULL ? 56+(int)i*276 : 196, 314, 248, 62};
        cl_round_rect(s, button, 10, selected == i ? v->theme->selected : v->theme->panel);
        fit_label(s, v, CL_FONT_LIST, (SDL_Rect){button.x+20, button.y+8, button.w-40, 46},
                  i == 0U ? cancel : confirm,
                  selected == i ? v->theme->selected_text : v->theme->fonts[CL_FONT_LIST].color);
    }
    cl_view_footer(s, v, cancel != NULL ? CL_TR("A Select     B Cancel") : CL_TR("A OK     B Back"));
}
static SDL_Surface *system_icon(struct cl_assets *assets, const char *system, bool expert)
{
    size_t count;
    const struct cl_icon_slot *slots;
    if (expert) {
        slots = cl_icon_slots(CL_ICON_EXPERT, &count);
        for (size_t i = 0U; i < count; ++i)
            if (strcmp(system, slots[i].id) == 0 && assets->expert[i] != NULL)
                return assets->expert[i];
    }
    slots = cl_icon_slots(CL_ICON_CONSOLE, &count);
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(system, slots[i].id) == 0) {
            return assets->systems[i];
        }
    }
    if (strstr(system, "Color") != NULL) {
        return assets->systems[1];
    }
    if (strstr(system, "Game Boy") != NULL) {
        return assets->systems[0];
    }
    return NULL;
}
void cl_view_gallery(SDL_Surface *s,const struct cl_view *v,const char *name,
                     const char *position,bool hide_bars)
{
    (void)SDL_FillRect(s,NULL,rgb(s,0U));
    fit(s,v->artwork,(SDL_Rect){0,hide_bars?0:48,640,hide_bars?480:384});
    if(v->artwork==NULL)
        fit_label(s,v,CL_FONT_LIST,(SDL_Rect){80,212,480,48},CL_TR("Screenshot unavailable"),v->theme->muted);
    if(hide_bars) return;
    const SDL_Rect header={0,0,640,48},footer={0,432,640,48};
    (void)SDL_FillRect(s,&header,rgb(s,v->theme->panel));
    (void)SDL_FillRect(s,&footer,rgb(s,v->theme->panel));
    cl_view_text(s,v->assets,CL_FONT_LIST,16,10,name,v->theme->fonts[CL_FONT_LIST].color,505);
    cl_view_text(s,v->assets,CL_FONT_HINT,527,14,position,v->theme->fonts[CL_FONT_HINT].color,100);
    cl_view_footer(s,v,CL_TR("LEFT/RIGHT Browse   Y Fullscreen   MENU Exit"));
}

void cl_view_draw(SDL_Surface *s, const struct cl_view *v)
{
    const struct cl_theme *t = v->theme;
    size_t i;
    char status[80];
    const struct cl_entry *selected = v->position->selected < v->history->count
                                          ? &v->history->entries[v->position->selected]
                                          : NULL;
    cl_view_frame(s, v, v->carousel ? "GameSwitcher" : (v->heading != NULL ? v->heading : CL_TR("Games")));
    if (selected == NULL) {
        cl_view_text(s, v->assets, CL_FONT_LIST, 28, 200,
                     v->carousel ? CL_TR("No recent games") : CL_TR("No games in this view"), t->muted, 584);
    } else if (v->carousel) {
        (void)SDL_FillRect(s, NULL, rgb(s, 0U));
        fit(s, v->artwork, (SDL_Rect){0, v->fullscreen ? 0 : 40, 640, v->fullscreen ? 480 : 400});
        if (v->artwork == NULL)
            fit_label(s, v, CL_FONT_LIST, (SDL_Rect){100, 200, 440, 48}, CL_TR("No screenshot available"), t->muted);
        if (!v->fullscreen) {
            SDL_Rect header = {0, 0, 640, 40}, footer = {0, 440, 640, 40};
            (void)SDL_FillRect(s, &header, rgb(s, t->panel));
            (void)SDL_FillRect(s, &footer, rgb(s, t->panel));
            image_rect(s, v->assets->switcher_top, header);
            image_rect(s, v->assets->switcher_bottom, footer);
            if (v->show_playtime)
                (void)snprintf(status, sizeof(status), "%lluh %02llum", (unsigned long long)(selected->total_play_seconds/3600U),
                               (unsigned long long)(selected->total_play_seconds/60U%60U));
            else (void)snprintf(status, sizeof(status), "GameSwitcher");
            fit_label(s, v, CL_FONT_TITLE, (SDL_Rect){180, 0, 310, 40}, status, t->fonts[CL_FONT_TITLE].color);
            (void)snprintf(status, sizeof(status), "%zu / %zu", v->position->selected+1U, v->history->count);
            fit_label(s, v, CL_FONT_DETAIL, (SDL_Rect){540, 0, 88, 40}, status, t->fonts[CL_FONT_TITLE].color);
            cl_round_rect(s, (SDL_Rect){0, 380, 640, 60}, 0, t->background);
            fit_label(s, v, CL_FONT_TITLE, (SDL_Rect){48, 388, 544, 44}, selected->title,
                      readable_ink(t->fonts[CL_FONT_TITLE].color, t->background));
            if (v->position->selected > 0U) fit(s, v->assets->arrow_left, (SDL_Rect){12, 398, 24, 24});
            if (v->position->selected+1U < v->history->count) fit(s, v->assets->arrow_right, (SDL_Rect){604, 398, 24, 24});
        }
    } else if (t->grid || v->systems) {
        int width = (640 - t->border_left - t->border_right - (t->grid_columns - 1) * t->grid_gap) /
                    t->grid_columns;
        int height = (328 - (t->grid_rows - 1) * t->grid_gap) / t->grid_rows;
        for (i = 0U; i < 6U && v->position->first + i < v->history->count; ++i) {
            size_t index = v->position->first + i;
            const struct cl_entry *e = &v->history->entries[index];
            SDL_Rect panel = {
                t->border_left + (int)(i % (size_t)t->grid_columns) * (width + t->grid_gap),
                t->list_top + (int)(i / (size_t)t->grid_columns) * (height + t->grid_gap), width,
                height};
            bool active = index == v->position->selected;
            uint32_t background = active ? t->selected : t->panel;
            uint32_t ink = readable_ink(active ? t->selected_text : t->fonts[CL_FONT_LIST].color,
                                       background);
            cl_round_rect(s, panel, t->radius, background);
            {
                SDL_Rect art = {panel.x + 8, panel.y + 6, panel.w - 16, panel.h - 64};
                fit(s,
                    v->systems ? (e->library_action == CL_LIBRARY_SEARCH_RESULTS ? v->assets->apps[CL_APP_ICON_SEARCH] :
                                  e->library_action == CL_LIBRARY_PORTMASTER ? v->assets->apps[15] :
                                  e->library_action == CL_LIBRARY_PORTS ? v->assets->apps[15] :
                                  system_icon(v->assets, e->system, v->expert)) :
                        (v->grid_artwork[i] != NULL ? v->grid_artwork[i] :
                         (active && v->artwork != NULL ? v->artwork : system_icon(v->assets, e->system, v->expert))),
                    art);
            }
            cl_view_text(s, v->assets, CL_FONT_GRID_SYSTEM, panel.x + 10, panel.y + panel.h - 50,
                         e->title, ink, panel.w - 20);
            cl_view_text(s, v->assets, CL_FONT_DETAIL, panel.x + 10, panel.y + panel.h - 25,
                         e->system, ink, panel.w - 20);
        }
    } else {
        for (i = 0U; i < 6U && v->position->first + i < v->history->count; ++i) {
            size_t index = v->position->first + i;
            const struct cl_entry *e = &v->history->entries[index];
            bool active = index == v->position->selected;
            int y = t->list_top + (int)i * t->row_height;
            SDL_Rect panel = {t->border_left, y, 370 - t->border_left,
                              t->row_height - t->row_gap};
            int text_x = panel.x + 12;
            uint32_t ink = active ? t->selected_text : t->fonts[CL_FONT_LIST].color;
            if (v->assets->list_item[active ? 1U : 0U] != NULL) {
                image_rect(s, v->assets->list_item[active ? 1U : 0U], panel);
            } else if (active) {
                cl_round_rect(s, panel, t->radius, t->selected);
            }
            cl_view_text(s, v->assets, CL_FONT_LIST, text_x, y + 10, e->title, ink,
                         panel.w - 24);
        }
        /* Onion's selected artwork belongs beside the list, never behind text.
         * The worker supplies only the current selection; no filesystem I/O here. */
        fit(s, v->artwork, (SDL_Rect){382, 100, 242, 252});
        if (v->artwork == NULL) {
            fit(s, system_icon(v->assets, selected->system, v->expert), (SDL_Rect){448, 154, 112, 112});
            
        }
        cl_view_text(s, v->assets, CL_FONT_DETAIL, 394, 366, selected->system, t->muted, 218);

    }
    if (!t->hide_hints && !v->carousel) {
        cl_view_footer(s, v, v->systems || (selected != NULL && selected->library_action != CL_LIBRARY_PLAY) ?
                       CL_TR("A Select     B Back") : CL_TR("A Play   B Back   X States   Y Options"));
        uint64_t current = selected != NULL ? (uint64_t)v->position->selected+1U : 0U;
        uint64_t total = (uint64_t)v->history->count;
        if (v->history->library_total != 0U) {
            current += v->history->library_offset;
            total = v->history->library_total;
        }
        (void)snprintf(status, sizeof(status), "%llu/%llu", (unsigned long long)current,
                       (unsigned long long)total);
        fit_label(s, v, CL_FONT_DETAIL, (SDL_Rect){538, 432, 86, 36}, status, t->muted);
    }

}

void cl_settings_draw(SDL_Surface *s, const struct cl_view *v, bool grid, size_t selected)
{
    const struct cl_theme *t = v->theme;
    const char *labels[] = {"Brightness", "Volume", "Wi-Fi", "Theme",
        grid ? "Games layout: Grid" : "Games layout: List", "Button settings",
        "About device", "Storage recovery", "Date & Time", "Language", "Power off"};
    size_t first = selected < 6U ? 0U : selected - 5U;
    cl_view_frame(s, v, CL_TR("Settings"));
    for (size_t i = first; i < 11U && i < first + 6U; ++i) {
        int y = t->list_top + (int)(i - first) * t->row_height;
        SDL_Rect r = {t->border_left, y, 640 - t->border_left - t->border_right,
                      t->row_height - t->row_gap};
        bool active = i == selected;
        if (v->assets->list_item[active ? 1U : 0U] != NULL)
            image_rect(s, v->assets->list_item[active ? 1U : 0U], r);
        else if (active)
            cl_round_rect(s, r, t->radius, t->selected);
        fit(s, v->assets->settings[i < 9U ? i : (i == 10U ? 9U : 6U)], (SDL_Rect){r.x + 8, y + 4, 40, 40});
        cl_view_text(s, v->assets, CL_FONT_LIST, r.x + 62, y + 9, CL_TR(labels[i]),
                     active ? t->selected_text : t->fonts[CL_FONT_LIST].color, r.w - 120);
        if (i < 2U) {
            char level[24];
            unsigned int value = i == 0U ? v->brightness_level : v->volume_level;
            if (value == 0U) strcpy(level, "< -- >");
            else (void)snprintf(level, sizeof(level), "< %02u/%u >", value - 1U, i == 0U ? 10U : 20U);
            fit_label(s, v, CL_FONT_DETAIL, (SDL_Rect){r.x + r.w - 128, y + 6, 116, 38},
                      level, active ? t->selected_text : t->fonts[CL_FONT_DETAIL].color);
        }
    }
    cl_view_footer(s, v, selected < 2U ? CL_TR("Left/Right Adjust     B Back") : CL_TR("A Select     B Back"));
}

void cl_options_draw(SDL_Surface *s, const struct cl_view *v, const char *title,
                     const char *const *labels, size_t count, size_t selected)
{
    size_t first = (selected / 6U) * 6U;
    cl_view_frame(s, v, title);
    for (size_t i = first; i < count && i < first+6U; ++i) {
        SDL_Rect row = {0, 60+(int)(i-first)*60, 640, 60};
        if (i == selected) {
            if (v->assets->list_item[1] != NULL) image_rect(s, v->assets->list_item[1], row);
            else cl_round_rect(s, row, v->theme->radius, v->theme->selected);
        }
        SDL_Surface *icon = v->option_icons != NULL ? v->option_icons[i] : NULL;
        if (icon != NULL) image_rect(s, icon, (SDL_Rect){20, row.y+8, 44, 44});
        int label_x = icon != NULL ? 76 : 30;
        fit_label(s, v, CL_FONT_LIST, (SDL_Rect){label_x, row.y+6, 610-label_x, 48}, labels[i],
                  i == selected ? v->theme->selected_text : v->theme->fonts[CL_FONT_LIST].color);
    }
    cl_view_footer(s, v, CL_TR("A Select     B Back"));
}

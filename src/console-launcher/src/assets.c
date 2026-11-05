#include "launcher/assets.h"
#include "SDL_ttf.h"
#include "lodepng.h"
#include "stb_image.h"
#include "sessiond/library.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CL_ASSET_TRACE
#define ASSET_TRACE(...) do { fprintf(stderr,__VA_ARGS__);fflush(stderr); } while(0)
#else
#define ASSET_TRACE(...) ((void)0)
#endif
static unsigned int le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8U);
}
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}
static int signed16(const unsigned char *p)
{
    unsigned int v = le16(p);
    return v > 32767U ? (int)v - 65536 : (int)v;
}
static unsigned char *read_asset(const char *path, size_t *size)
{
    int fd = cs_library_open_absolute(path, false);
    struct stat st;
    unsigned char *bytes = NULL;
    size_t used = 0U;
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1 ||
        st.st_size > 8 * 1024 * 1024) {
        goto done;
    }
    bytes = malloc((size_t)st.st_size);
    if (bytes == NULL) {
        goto done;
    }
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            free(bytes);
            bytes = NULL;
            goto done;
        }
        used += (size_t)n;
    }
    *size = used;
done:
    if (close(fd) != 0) {
        free(bytes);
        bytes = NULL;
    }
    return bytes;
}
SDL_Surface *cl_image_read(const char *path)
{
    size_t size;
    unsigned char *bytes = read_asset(path, &size);
    SDL_Surface *surface = NULL;
    if (bytes == NULL) {
        return NULL;
    }
    if (size >= 8U && memcmp(bytes, "\x89PNG\r\n\x1a\n", 8U) == 0) {
        unsigned int width, height;
        unsigned char *rgba = NULL;
        LodePNGState state;
        lodepng_state_init(&state);
        state.decoder.zlibsettings.max_output_size = 8U * 1024U * 1024U;
        state.decoder.max_text_size = 65536U;
        state.decoder.max_icc_size = 65536U;
        if (lodepng_inspect(&width, &height, &state, bytes, size) == 0U && width > 0U &&
            height > 0U && width <= 2048U && height <= 2048U &&
            (uint64_t)width * height <= 1048576U &&
            lodepng_decode(&rgba, &width, &height, &state, bytes, size) == 0U) {
            size_t y, x;
            surface = SDL_CreateRGBSurfaceWithFormat(0, (int)width, (int)height, 32,
                                                     SDL_PIXELFORMAT_ARGB8888);
            if (surface != NULL) {
                for (y = 0U; y < height; ++y) {
                    Uint32 *row =
                        (Uint32 *)((unsigned char *)surface->pixels + y * (size_t)surface->pitch);
                    for (x = 0U; x < width; ++x) {
                        const unsigned char *p = rgba + 4U * (y * width + x);
                        row[x] = SDL_MapRGBA(surface->format, p[0], p[1], p[2], p[3]);
                    }
                }
                (void)SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
            }
        }
        free(rgba);
        lodepng_state_cleanup(&state);
    } else if (size >= 3U && bytes[0] == 0xffU && bytes[1] == 0xd8U && bytes[2] == 0xffU) {
        int width, height, components;
        if (stbi_info_from_memory(bytes, (int)size, &width, &height, &components) &&
            width > 0 && height > 0 && width <= 2048 && height <= 2048 && width * height <= 1048576) {
            unsigned char *pixels = stbi_load_from_memory(bytes, (int)size, &width, &height, &components, 3);
            if (pixels != NULL) {
                SDL_Surface *rgb24 = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height, 24,
                                                                       width * 3, SDL_PIXELFORMAT_RGB24);
                if (rgb24 != NULL) {
                    surface = SDL_ConvertSurfaceFormat(rgb24, SDL_PIXELFORMAT_ARGB8888, 0U);
                    SDL_FreeSurface(rgb24);
                }
                stbi_image_free(pixels);
            }
        }
    } else if (size >= 54U && bytes[0] == 'B' && bytes[1] == 'M') {
        uint32_t width = le32(bytes + 18U), height = le32(bytes + 22U);
        if (width > 0U && height > 0U && width <= 2048U && height <= 2048U &&
            (uint64_t)width * height <= 1048576U) {
            SDL_RWops *rw = SDL_RWFromConstMem(bytes, (int)size);
            if (rw != NULL) {
                surface = SDL_LoadBMP_RW(rw, 1);
            }
        }
    }
    free(bytes);
    return surface;
}
static bool font_open(struct cl_atlas *font, const char *root, const struct cl_font_style *style)
{
    char path[768];
    unsigned char *data;
    size_t size, i;
    bool ok = false;
    if (snprintf(path, sizeof(path), "%s/%s.%d.metrics", root, style->font, style->size) >=
        (int)sizeof(path)) {
        return false;
    }
    data = read_asset(path, &size);
    if (data == NULL) {
        return false;
    }
    if (size < 12U || memcmp(data, "MXFA", 4U) != 0 || le16(data + 4U) != 1U) {
        goto done;
    }
    font->count = le16(data + 6U);
    font->height = (int)le16(data + 8U);
    if (font->count > 256U || font->height < 1 || font->height > 64 ||
        size != 12U + 18U * font->count) {
        goto done;
    }
    if (snprintf(path, sizeof(path), "%s/%s.%d.png", root, style->font, style->size) >=
        (int)sizeof(path)) {
        goto done;
    }
    font->surface = cl_image_read(path);
    if (font->surface == NULL) {
        goto done;
    }
    for (i = 0U; i < font->count; ++i) {
        const unsigned char *p = data + 12U + 18U * i;
        struct cl_atlas_glyph *g = &font->glyphs[i];
        g->code = le32(p);
        g->rect.x = (int)le16(p + 4U);
        g->rect.y = (int)le16(p + 6U);
        g->rect.w = (int)le16(p + 8U);
        g->rect.h = (int)le16(p + 10U);
        g->xoff = signed16(p + 12U);
        g->yoff = signed16(p + 14U);
        g->advance = (int)le16(p + 16U);
        if (g->rect.x + g->rect.w > font->surface->w || g->rect.y + g->rect.h > font->surface->h ||
            g->advance < 1 || g->advance > 64 || g->xoff < -32 || g->xoff > 32 || g->yoff < -32 ||
            g->yoff > 64) {
            goto done;
        }
    }
    ok = true;
done:
    free(data);
    if (!ok) {
        SDL_FreeSurface(font->surface);
        memset(font, 0, sizeof(*font));
    }
    return ok;
}
static bool font_path(char path[768], const char *root, const char *font)
{
    const char *relative = font;
    char rebased[160];
    if (strncmp(font, "/mnt/SDCARD/miyoo/app/", 22U) == 0) {
        const char *name = font + 22U;
        if (name[0] == '\0' || strchr(name, '/') != NULL || strstr(name, "..") != NULL) {
            return false;
        }
        if (snprintf(rebased, sizeof(rebased), "fonts/%s", name) >= (int)sizeof(rebased)) {
            return false;
        }
        relative = rebased;
    }
    return snprintf(path, 768U, "%s/%s", root, relative) < 768;
}
static bool runtime_font_open(struct cl_assets *assets, size_t index, const char *root,
                              const struct cl_theme *theme)
{
    char path[768];
    size_t size = 0U;
    unsigned char *bytes = NULL;
    bool owns_bytes = true;
    const struct cl_font_style *style = &theme->fonts[index];
    struct cl_runtime_font *font = &assets->runtime_fonts[index];
    if (!assets->ttf_initialized || !font_path(path, root, style->font)) return false;
    for (size_t i = 0U; i < index; ++i) {
        const struct cl_runtime_font *previous = &assets->runtime_fonts[i];
        if (previous->bytes != NULL && strcmp(previous->source, path) == 0) {
            if (previous->font != NULL && theme->fonts[i].size == style->size) {
                *font = *previous;
                font->owns_font = false; font->owns_bytes = false;
                ASSET_TRACE("FONT_SHARE_FACE index=%zu previous=%zu\n", index, i);
                return true;
            }
            if (bytes == NULL) {
                bytes = previous->bytes; size = previous->bytes_size; owns_bytes = false;
                ASSET_TRACE("FONT_SHARE_BYTES index=%zu previous=%zu\n", index, i);
            }
        }
    }
    if (bytes == NULL) {
        ASSET_TRACE("FONT_READ index=%zu path=%s\n",index,path);
        bytes = read_asset(path, &size);
        if (bytes == NULL && strncmp(style->font, "/mnt/SDCARD/miyoo/app/", 22U) == 0 &&
            font_path(path, "/system/share/console/themes/Silky", style->font))
            bytes = read_asset(path, &size);
    }
    if (bytes == NULL || size > 8U * 1024U * 1024U) {
        if (owns_bytes) free(bytes);
        return false;
    }
    {
        SDL_RWops *stream = SDL_RWFromConstMem(bytes, (int)size);
        if (stream != NULL) font->font = TTF_OpenFontRW(stream, 1, style->size);
    }
    if (font->font == NULL || TTF_FontHeight(font->font) < 1 || TTF_FontHeight(font->font) > 72) {
        if (font->font != NULL) TTF_CloseFont(font->font);
        memset(font, 0, sizeof(*font));
        if (owns_bytes) free(bytes);
        return false;
    }
    font->bytes = bytes; font->bytes_size = size;
    strcpy(font->source, path);
    font->owns_bytes = owns_bytes; font->owns_font = true;
    return true;
}
static SDL_Surface *image_under(const char *root, const char *name)
{
    char path[768];
    int n=snprintf(path,sizeof(path),"%s/%s",root,name);
    return n>0 && (size_t)n<sizeof(path) ? cl_image_read(path) : NULL;
}
static SDL_Surface *skin_image(const char *root, const char *name)
{
    SDL_Surface *image=NULL;
    ASSET_TRACE("SKIN_IMAGE %s\n",name);
    if (strncmp(name,"skin/",5U)==0) {
        image=image_under("/mnt/SDCARD/Saves/CurrentProfile/theme",name);
        if (image==NULL) image=image_under(root,name);
        if (image==NULL && strncmp(name,"skin/extra/",11U)==0)
            image=image_under("/mnt/SDCARD/.tmp_update/res",name+11U);
        else if (image==NULL) image=image_under("/mnt/SDCARD/miyoo/app",name);
    } else {
        image=image_under(root,name);
        if (image==NULL && strncmp(name,"icons/",6U)==0)
            image=image_under("/mnt/SDCARD/Icons/Default",name+6U);
    }
    /* Protected bundled artwork keeps an unavailable/removable default pack
     * from turning unrelated theme omissions into an empty interface. */
    if (image==NULL && strcmp(root,"/system/share/console/themes/Silky")!=0)
        image=image_under("/system/share/console/themes/Silky",name);
    return image;
}
void cl_assets_apply_icon_pack(struct cl_assets *a, const char *theme, const char *pack,
                               const char *card)
{
    SDL_Surface **const destination[] = {a->systems, a->apps, a->expert};
    static const char *const folders[] = {"", "app/", "expert/"};
    for (unsigned int group = 0U; group < CL_ICON_GROUP_COUNT; ++group) {
        size_t count;
        const struct cl_icon_slot *slots = cl_icon_slots((enum cl_icon_group)group, &count);
        for (size_t i = 0U; i < count; ++i) {
            char path[160];
            SDL_Surface *icon = NULL;
            if (card != NULL && card[0] != '\0') {
                (void)snprintf(path, sizeof(path), ".console/icon-%s-%s.png",
                               cl_icon_group_prefix((enum cl_icon_group)group), slots[i].id);
                icon = image_under(card, path);
            }
            (void)snprintf(path, sizeof(path), "icons/%s%s.png", folders[group], slots[i].stem);
            if (icon == NULL && pack[0] != '\0') icon = image_under(pack, path + 6U);
            if (icon == NULL && group == CL_ICON_EXPERT) {
                char alternate[160];
                (void)snprintf(alternate, sizeof(alternate), "icons/rapp/%s.png", slots[i].stem);
                icon = pack[0] != '\0' ? image_under(pack, alternate + 6U) : image_under(theme, alternate);
            }
            if (icon == NULL) icon = skin_image(theme, path);
            if (icon == NULL && group == CL_ICON_EXPERT) {
                (void)snprintf(path, sizeof(path), "icons/%s.png", slots[i].stem);
                if (pack[0] != '\0') icon = image_under(pack, path + 6U);
                if (icon == NULL) icon = skin_image(theme, path);
            }
            SDL_FreeSurface(destination[group][i]);
            destination[group][i] = icon;
        }
    }
}

bool cl_assets_open(struct cl_assets *a, const struct cl_theme *theme, const char *root)
{
    static const char *const tab_names[] = {"favorite", "game", "app", "setting", "recent"};
    static const char *const power[] = {"power-0%-icon.png", "power-20%-icon.png",
                                        "power-50%-icon.png", "power-80%-icon.png",
                                        "power-full-icon.png"};
    static const char *const setting_icons[] = {"icon-brightness-48.png", "sound-icon.png",
        "icon-setting-wifi.png", "icon-theme.png", "icon-game.png",
        "icon-key-setting-48.png", "icon-device-info-48.png", "icon-TF.png", "icon-device-info-48.png", "icon-Shutdown.png"};
    size_t i, j;
    char path[160];
    bool fonts = true;
    Uint64 started_at = SDL_GetTicks64();
    memset(a, 0, sizeof(*a));
    ASSET_TRACE("TTF_INIT_BEGIN\n");
    a->ttf_initialized = TTF_Init() == 0;
    ASSET_TRACE("TTF_INIT_DONE ok=%d\n",a->ttf_initialized ? 1 : 0);
    for (i = 0U; i < CL_FONT_COUNT; ++i) {
        if (!runtime_font_open(a, i, root, theme) &&
            !font_open(&a->fonts[i], root, &theme->fonts[i])) {
            fonts = false;
        }
    }
    a->background = skin_image(root, "skin/background.png");
    if (a->background == NULL) {
        a->background = skin_image(root, "skin/bg-gamelist.png");
    }
    a->title = skin_image(root, "skin/bg-title.png");
    a->footer = skin_image(root, "skin/tips-bar-bg.png");
    for (i = 0U; i < 10U; ++i) {
        (void)snprintf(path, sizeof(path), "skin/%s", setting_icons[i]);
        a->settings[i] = skin_image(root, path);
    }
    for (i = 0U; i < 2U; ++i) {
        (void)snprintf(path, sizeof(path), "skin/bg-game-item-%s.png", i == 0U ? "n" : "f");
        a->list_item[i] = i == 1U ? skin_image(root, "skin/bg-list-s.png") : NULL;
        (void)snprintf(path, sizeof(path), "skin/bg-button-%s.png", i == 0U ? "n" : "f");
        a->button[i] = skin_image(root, path);
    }
    for (i = 0U; i < 5U; ++i) {
        for (j = 0U; j < 2U; ++j) {
            (void)snprintf(path, sizeof(path), "skin/ic-%s-%s.png", tab_names[i],
                           j == 0U ? "n" : "f");
            a->tabs[i][j] = skin_image(root, path);
        }
    }
    for (i = 0U; i < 5U; ++i) {
        (void)snprintf(path, sizeof(path), "skin/%s", power[i]);
        a->battery[i] = skin_image(root, path);
    }
    a->charging = skin_image(root, "skin/ic-power-charge-100%.png");
    a->popup = skin_image(root, "skin/pop-bg.png");
    a->hint_a = skin_image(root, "skin/icon-A-54.png");
    a->hint_b = skin_image(root, "skin/icon-B-54.png");
    a->arrow_left = skin_image(root, "skin/icon-left-arrow-24.png");
    a->arrow_right = skin_image(root, "skin/icon-right-arrow-24.png");
    a->switcher_top = skin_image(root, "skin/extra/gs-top-bar.png");
    a->switcher_bottom = skin_image(root, "skin/extra/gs-bottom-bar.png");
    cl_assets_apply_icon_pack(a, root, "", NULL);
    {
        size_t bytes = 0U, faces = 0U;
        for (i = 0U; i < CL_FONT_COUNT; ++i) {
            if (a->runtime_fonts[i].owns_bytes) bytes += a->runtime_fonts[i].bytes_size;
            if (a->runtime_fonts[i].owns_font) ++faces;
        }
        fprintf(stderr, "launcher assets: open_ms=%llu font_bytes=%zu font_faces=%zu\n",
                (unsigned long long)(SDL_GetTicks64() - started_at), bytes, faces);
    }
    return fonts;
}
void cl_assets_close(struct cl_assets *a)
{
    size_t i, j;
    for (i = 0U; i < CL_FONT_COUNT; ++i) {
        SDL_FreeSurface(a->fonts[i].surface);
        if (a->runtime_fonts[i].font != NULL && a->runtime_fonts[i].owns_font) {
            TTF_CloseFont(a->runtime_fonts[i].font);
        }
    }
    /* Every face must be closed before freeing any shared backing bytes. */
    for (i = 0U; i < CL_FONT_COUNT; ++i)
        if (a->runtime_fonts[i].owns_bytes) free(a->runtime_fonts[i].bytes);
    for (i = 0U; i < CL_EXPERT_ICON_COUNT; ++i) SDL_FreeSurface(a->expert[i]);
    SDL_FreeSurface(a->background);
    SDL_FreeSurface(a->title);
    SDL_FreeSurface(a->footer);
    for (i = 0U; i < 10U; ++i)
        SDL_FreeSurface(a->settings[i]);
    for (i = 0U; i < 2U; ++i) {
        SDL_FreeSurface(a->list_item[i]);
        SDL_FreeSurface(a->button[i]);
    }
    for (i = 0U; i < 5U; ++i) {
        for (j = 0U; j < 2U; ++j) {
            SDL_FreeSurface(a->tabs[i][j]);
        }
    }
    for (i = 0U; i < 5U; ++i) {
        SDL_FreeSurface(a->battery[i]);
    }
    for (i = 0U; i < CL_SYSTEM_ICON_COUNT; ++i) {
        SDL_FreeSurface(a->systems[i]);
    }
    for (i = 0U; i < CL_APP_ICON_COUNT; ++i) {
        SDL_FreeSurface(a->apps[i]);
    }
    SDL_FreeSurface(a->charging);
    SDL_FreeSurface(a->popup);
    SDL_FreeSurface(a->hint_a); SDL_FreeSurface(a->hint_b);
    SDL_FreeSurface(a->arrow_left); SDL_FreeSurface(a->arrow_right);
    SDL_FreeSurface(a->switcher_top);
    SDL_FreeSurface(a->switcher_bottom);
    if (a->ttf_initialized) {
        ASSET_TRACE("TTF_QUIT_BEGIN\n");
        TTF_Quit();
    }
    memset(a, 0, sizeof(*a));
}
bool cl_runtime_text(struct cl_assets *a, enum cl_theme_role role, SDL_Surface *target, int x,
                     int y, const char *text, uint32_t color, int width)
{
    SDL_Surface *rendered;
    SDL_Rect old, clip, dst = {x, y, 0, 0};
    char bounded[512];
    size_t bytes;
    int measured_width, measured_height;
    SDL_Color ink = {(Uint8)(color >> 16U), (Uint8)(color >> 8U), (Uint8)color, 255U};
    if (role < 0 || role >= CL_FONT_COUNT || a->runtime_fonts[role].font == NULL || width < 1) {
        return false;
    }
    bytes = strnlen(text, sizeof(bounded));
    if (bytes >= sizeof(bounded)) {
        return false;
    }
    memcpy(bounded, text, bytes + 1U);
    if (TTF_SizeUTF8(a->runtime_fonts[role].font, bounded, &measured_width, &measured_height) !=
            0 ||
        measured_width < 0 || measured_width > 8192 || measured_height < 1 ||
        measured_height > 72) {
        return false;
    }
    rendered = TTF_RenderUTF8_Blended(a->runtime_fonts[role].font, bounded, ink);
    if (rendered == NULL) {
        return false;
    }
    SDL_GetClipRect(target, &old);
    clip = (SDL_Rect){x, y, width, measured_height};
    (void)SDL_IntersectRect(&clip, &old, &clip);
    (void)SDL_SetClipRect(target, &clip);
    (void)SDL_BlitSurface(rendered, NULL, target, &dst);
    (void)SDL_SetClipRect(target, &old);
    SDL_FreeSurface(rendered);
    return true;
}
static uint32_t next_code(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t c = *p++;
    unsigned int n = 0U, i;
    if (c >= 0xc2U && c <= 0xdfU) {
        n = 1U;
        c &= 31U;
    } else if (c >= 0xe0U && c <= 0xefU) {
        n = 2U;
        c &= 15U;
    } else if (c >= 0xf0U && c <= 0xf4U) {
        n = 3U;
        c &= 7U;
    } else if (c >= 0x80U) {
        c = 0xfffdU;
    }
    for (i = 0U; i < n; ++i) {
        if ((*p & 0xc0U) != 0x80U) {
            c = 0xfffdU;
            break;
        }
        c = (c << 6U) | (*p++ & 63U);
    }
    *text = (const char *)p;
    return c;
}
bool cl_text_measure(const struct cl_assets *a, enum cl_theme_role role, const char *text,
                     int *width, int *height)
{
    int w=0,h;
    size_t bytes;
    if (a==NULL || text==NULL || width==NULL || height==NULL ||
        (unsigned int)role>=CL_FONT_COUNT) return false;
    bytes=strnlen(text,512U);
    if (bytes>=512U) return false;
    if (a->runtime_fonts[role].font!=NULL) {
        if (TTF_SizeUTF8(a->runtime_fonts[role].font,text,&w,&h)!=0 ||
            w<0 || w>8192 || h<1 || h>72) return false;
    } else if (a->fonts[role].surface!=NULL) {
        const struct cl_atlas *font=&a->fonts[role];
        const char *p=text;
        h=font->height;
        while (*p!='\0') {
            uint32_t code=next_code(&p);
            const struct cl_atlas_glyph *glyph=NULL,*replacement=NULL;
            for(size_t i=0U;i<font->count;++i) {
                if(font->glyphs[i].code==code) glyph=&font->glyphs[i];
                if(font->glyphs[i].code==0xfffdU) replacement=&font->glyphs[i];
            }
            if(glyph==NULL) glyph=replacement;
            w+=glyph!=NULL?glyph->advance:font->height/2;
            if(w<0 || w>8192) return false;
        }
        if(h<1 || h>72) return false;
    } else {
        w=(int)bytes*(role==CL_FONT_DETAIL?6:12);
        h=role==CL_FONT_DETAIL?8:16;
    }
    *width=w;*height=h;return true;
}
void cl_atlas_text(struct cl_atlas *font, SDL_Surface *target, int x, int y, const char *text,
                   uint32_t color, size_t max_glyphs, int width)
{
    SDL_Rect old, clip;
    size_t count = 0U;
    if (font->surface == NULL || width < 1) {
        return;
    }
    SDL_GetClipRect(target, &old);
    clip.x = x;
    clip.y = y;
    clip.w = width;
    clip.h = font->height;
    (void)SDL_IntersectRect(&clip, &old, &clip);
    (void)SDL_SetClipRect(target, &clip);
    (void)SDL_SetSurfaceColorMod(font->surface, (Uint8)(color >> 16U), (Uint8)(color >> 8U),
                                 (Uint8)color);
    while (*text != '\0' && count++ < max_glyphs && x < clip.x + clip.w) {
        uint32_t c = next_code(&text);
        size_t i;
        struct cl_atlas_glyph *glyph = NULL, *replacement = NULL;
        for (i = 0U; i < font->count; ++i) {
            if (font->glyphs[i].code == c) {
                glyph = &font->glyphs[i];
            }
            if (font->glyphs[i].code == 0xfffdU) {
                replacement = &font->glyphs[i];
            }
        }
        if (glyph == NULL) {
            glyph = replacement;
        }
        if (glyph != NULL) {
            SDL_Rect dst = {x + glyph->xoff, y + glyph->yoff, 0, 0};
            (void)SDL_BlitSurface(font->surface, &glyph->rect, target, &dst);
            x += glyph->advance;
        }
    }
    (void)SDL_SetClipRect(target, &old);
}
void cl_round_rect(SDL_Surface *target, SDL_Rect rect, int radius, uint32_t color)
{
    int y;
    Uint32 pixel =
        SDL_MapRGB(target->format, (Uint8)(color >> 16U), (Uint8)(color >> 8U), (Uint8)color);
    if (radius < 0) {
        radius = 0;
    }
    if (radius > rect.h / 2) {
        radius = rect.h / 2;
    }
    if (radius > rect.w / 2) {
        radius = rect.w / 2;
    }
    for (y = 0; y < rect.h; ++y) {
        int inset = 0,
            dy = y < radius ? radius - y - 1 : (y >= rect.h - radius ? y - (rect.h - radius) : 0);
        while (inset < radius && (radius - inset) * (radius - inset) + dy * dy > radius * radius) {
            ++inset;
        }
        {
            SDL_Rect row = {rect.x + inset, rect.y + y, rect.w - 2 * inset, 1};
            (void)SDL_FillRect(target, &row, pixel);
        }
    }
}
struct cl_art_worker {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *condition;
    char path[512];
    uint64_t requested, completed;
    bool pending, ready, stop;
    SDL_Surface *result;
    char grid_paths[6][512];
    SDL_Surface *grid_results[6];
    bool batch;
};
static int art_thread(void *context)
{
    struct cl_art_worker *w = context;
    for (;;) {
        char path[512];
        uint64_t serial;
        SDL_Surface *image;
        char paths[6][512];
        SDL_Surface *images[6] = {0};
        bool batch;
        (void)SDL_LockMutex(w->mutex);
        while (!w->stop && !w->pending) {
            (void)SDL_CondWait(w->condition, w->mutex);
        }
        if (w->stop) {
            SDL_UnlockMutex(w->mutex);
            break;
        }
        strcpy(path, w->path);
        batch = w->batch;
        memcpy(paths, w->grid_paths, sizeof(paths));
        serial = w->requested;
        w->pending = false;
        SDL_UnlockMutex(w->mutex);
        image = batch || path[0] == '\0' ? NULL : cl_image_read(path);
        if (batch)
            for (size_t i = 0U; i < 6U; ++i)
                images[i] = paths[i][0] == '\0' ? NULL : cl_image_read(paths[i]);
        (void)SDL_LockMutex(w->mutex);
        if (serial == w->requested && !w->stop) {
            SDL_FreeSurface(w->result);
            w->result = image;
            image = NULL;
            for (size_t i = 0U; i < 6U; ++i) {
                SDL_FreeSurface(w->grid_results[i]);
                w->grid_results[i] = images[i]; images[i] = NULL;
            }
            w->completed = serial;
            w->ready = true;
        }
        SDL_UnlockMutex(w->mutex);
        SDL_FreeSurface(image);
        for (size_t i = 0U; i < 6U; ++i) SDL_FreeSurface(images[i]);
    }
    return 0;
}
struct cl_art_worker *cl_art_start(void)
{
    struct cl_art_worker *w = calloc(1U, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->mutex = SDL_CreateMutex();
    w->condition = SDL_CreateCond();
    if (w->mutex != NULL && w->condition != NULL) {
        w->thread = SDL_CreateThread(art_thread, "cover-art", w);
    }
    if (w->thread == NULL) {
        cl_art_stop(w);
        return NULL;
    }
    return w;
}
bool cl_art_request(struct cl_art_worker *w, const char *path)
{
    if (w == NULL || strlen(path) >= sizeof(w->path)) {
        return false;
    }
    (void)SDL_LockMutex(w->mutex);
    if (w->requested == UINT64_MAX) {
        SDL_UnlockMutex(w->mutex);
        return false;
    }
    strcpy(w->path, path);
    w->batch = false;
    ++w->requested;
    w->pending = true;
    w->ready = false;
    SDL_FreeSurface(w->result);
    w->result = NULL;
    SDL_CondSignal(w->condition);
    SDL_UnlockMutex(w->mutex);
    return true;
}
bool cl_art_take(struct cl_art_worker *w, SDL_Surface **surface)
{
    bool ready;
    if (w == NULL) {
        return false;
    }
    (void)SDL_LockMutex(w->mutex);
    ready = w->ready && !w->batch && w->completed == w->requested;
    if (ready) {
        *surface = w->result;
        w->result = NULL;
        w->ready = false;
    }
    SDL_UnlockMutex(w->mutex);
    return ready;
}
bool cl_art_request_grid(struct cl_art_worker *w, const char *const paths[6])
{
    if (w == NULL || paths == NULL) return false;
    for (size_t i = 0U; i < 6U; ++i)
        if (paths[i] == NULL || strlen(paths[i]) >= 512U) return false;
    (void)SDL_LockMutex(w->mutex);
    if (w->requested == UINT64_MAX) { SDL_UnlockMutex(w->mutex); return false; }
    for (size_t i = 0U; i < 6U; ++i) {
        strcpy(w->grid_paths[i], paths[i]);
        SDL_FreeSurface(w->grid_results[i]); w->grid_results[i] = NULL;
    }
    w->batch = true; ++w->requested; w->pending = true; w->ready = false;
    SDL_CondSignal(w->condition); SDL_UnlockMutex(w->mutex); return true;
}
bool cl_art_take_grid(struct cl_art_worker *w, SDL_Surface *surfaces[6])
{
    bool ready;
    if (w == NULL || surfaces == NULL) return false;
    (void)SDL_LockMutex(w->mutex);
    ready = w->ready && w->batch && w->completed == w->requested;
    if (ready) {
        for (size_t i = 0U; i < 6U; ++i) {
            SDL_FreeSurface(surfaces[i]); surfaces[i] = w->grid_results[i]; w->grid_results[i] = NULL;
        }
        w->ready = false;
    }
    SDL_UnlockMutex(w->mutex); return ready;
}
void cl_art_stop(struct cl_art_worker *w)
{
    if (w == NULL) {
        return;
    }
    if (w->thread != NULL) {
        (void)SDL_LockMutex(w->mutex);
        w->stop = true;
        SDL_CondSignal(w->condition);
        SDL_UnlockMutex(w->mutex);
        SDL_WaitThread(w->thread, NULL);
    }
    SDL_FreeSurface(w->result);
    for (size_t i = 0U; i < 6U; ++i) SDL_FreeSurface(w->grid_results[i]);
    SDL_DestroyCond(w->condition);
    SDL_DestroyMutex(w->mutex);
    free(w);
}

#ifdef NDEBUG
#error "Regression fixtures require active assertions; compile with -UNDEBUG"
#endif
#include "launcher/view.h"
#include "launcher/tools.h"
#include "launcher/input.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#define O_BINARY 0
#endif
/* Trusted isolated rendering fixture; production uses component-wise opens. */
int cs_library_open_absolute(const char *path, bool directory)
{
    (void)directory;
    return open(path, O_RDONLY | O_BINARY);
}
static void assert_switcher_logo(SDL_Surface *surface, const struct cl_theme *theme,
                                 const struct cl_assets *assets)
{
    SDL_Surface *reference;
    SDL_Rect header = {0, 0, 640, 40};
    int y;
    bool status_changed = false;
    reference = SDL_CreateRGBSurfaceWithFormat(0, 640, 40, 16, SDL_PIXELFORMAT_RGB565);
    assert(reference != NULL);
    assert(SDL_FillRect(reference, NULL,
                        SDL_MapRGB(reference->format, (Uint8)(theme->panel >> 16U),
                                   (Uint8)(theme->panel >> 8U), (Uint8)theme->panel)) == 0);
    assert(assets->switcher_top != NULL);
    assert(SDL_BlitScaled(assets->switcher_top, NULL, reference, &header) == 0);
    assert(SDL_LockSurface(reference) == 0);
    assert(SDL_LockSurface(surface) == 0);
    for (y = 0; y < 40; ++y) {
        const unsigned char *expected = (const unsigned char *)reference->pixels + y * reference->pitch;
        const unsigned char *actual = (const unsigned char *)surface->pixels + y * surface->pitch;
        assert(memcmp(expected, actual, 180U * 2U) == 0);
        if (memcmp(expected + 180U * 2U, actual + 180U * 2U, (616U - 180U) * 2U) != 0)
            status_changed = true;
        assert(memcmp(expected + 616U * 2U, actual + 616U * 2U, (640U - 616U) * 2U) == 0);
    }
    assert(status_changed);
    SDL_UnlockSurface(surface);
    SDL_UnlockSurface(reference);
    SDL_FreeSurface(reference);
}
int main(int argc, char **argv)
{
    struct cl_theme theme, before;
    struct cl_assets assets;
    struct cl_history history;
    struct cl_position position;
    struct cl_view view;
    SDL_Surface *surface;
    size_t i;
    char path[1024];
    static const char *const titles[] = {"Pokemon - Crystal", "Pokemon - Blue",
                                         "Pokemon - Gold",    "Pokemon - Red",
                                         "Pokemon - Silver",  "Pokemon - Yellow"};
    const char *bad = "{\"layout\":{\"rowHeight\":1000}}",
               *good = "{\"frame\":{\"border-left\":24}}";
    fprintf(stderr,"SILKY_PHASE_MAIN\n");fflush(stderr);
    if (argc != 3) {
        return 2;
    }
    cl_theme_default(&theme);
    assert(cl_theme_valid(&theme));
    before = theme;
    assert(!cl_theme_apply(&theme, bad, strlen(bad)));
    assert(memcmp(&theme, &before, sizeof(theme)) == 0);
    assert(cl_theme_apply(&theme, good, strlen(good)));
    assert(theme.border_left == 24);
    cl_theme_default(&theme);
    (void)snprintf(path, sizeof(path), "%s/config.json", argv[1]);
    assert(cl_theme_read(&theme, path));
    (void)snprintf(path, sizeof(path), "%s/moss.override.json", argv[1]);
    (void)cl_theme_read(&theme, path);
    fprintf(stderr,"SILKY_PHASE_SDL_INIT\n");fflush(stderr);
    assert(SDL_Init(0U) == 0);
    fprintf(stderr,"SILKY_PHASE_ASSETS_OPEN\n");fflush(stderr);
    assert(cl_assets_open(&assets, &theme, argv[1]));
    {
        size_t blobs=0U,faces=0U,bytes=0U,logical_bytes=0U;
        for(size_t role=0U;role<CL_FONT_COUNT;++role){
            const struct cl_runtime_font *font=&assets.runtime_fonts[role];
            if(font->font==NULL)continue;
            assert(font->bytes!=NULL&&font->bytes_size>0U);
            logical_bytes+=font->bytes_size;
            if(font->owns_bytes){++blobs;bytes+=font->bytes_size;}
            if(font->owns_font)++faces;
            for(size_t before_role=0U;before_role<role;++before_role){
                const struct cl_runtime_font *earlier=&assets.runtime_fonts[before_role];
                if(earlier->font==NULL||strcmp(earlier->source,font->source)!=0)continue;
                assert(font->bytes==earlier->bytes&&!font->owns_bytes);
                if(theme.fonts[before_role].size==theme.fonts[role].size)
                    assert(font->font==earlier->font&&!font->owns_font);
            }
        }
        assert(blobs>0U&&faces>=blobs&&bytes<logical_bytes);
        printf("FONT_REUSE_PASS blobs=%zu faces=%zu bytes=%zu unshared_bytes=%zu\n",blobs,faces,bytes,logical_bytes);
    }
    if (strcmp(theme.name, "Silky") == 0) {
        for (i = 0U; i < CL_FONT_COUNT; ++i) {
            assert(assets.runtime_fonts[i].font != NULL);
        }
        for (i = 0U; i < 5U; ++i) {
            assert(assets.tabs[i][0] != NULL && assets.tabs[i][1] != NULL);
        }
        assert(assets.background != NULL);
    }
    fprintf(stderr,"SILKY_PHASE_SURFACE_CREATE\n");fflush(stderr);
    surface = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 16, SDL_PIXELFORMAT_RGB565);
    assert(surface != NULL);
    memset(&history, 0, sizeof(history));
    memset(&position, 0, sizeof(position));
    history.count = 6U;
    for (i = 0U; i < 6U; ++i) {
        strcpy(history.entries[i].title, titles[i]);
        strcpy(history.entries[i].system, "GBC");
        history.entries[i].state_status = CL_READY;
    }
    view = (struct cl_view){.theme = &theme,
                            .assets = &assets,
                            .history = &history,
                            .position = &position,
                            .heading = "Games",
                            .battery = 78};
    for (i = 0U; i < 5U; ++i) {
        fprintf(stderr,"SILKY_PHASE_MAIN_DRAW=%zu\n",i);fflush(stderr);
        cl_main_draw(surface, &view, (unsigned int)i);
        (void)snprintf(path, sizeof(path), "%s/main-%zu.bmp", argv[2], i);
        assert(SDL_SaveBMP(surface, path) == 0);
    }
    cl_view_draw(surface, &view);
    (void)snprintf(path, sizeof(path), "%s/list.bmp", argv[2]);
    assert(SDL_SaveBMP(surface, path) == 0);
    view.artwork = cl_image_read("/system/share/console/box-art/GBC/Imgs/Pokemon - Crystal.png");
    assert(view.artwork != NULL);
    cl_view_draw(surface, &view);
    (void)snprintf(path, sizeof(path), "%s/list-art.bmp", argv[2]);
    assert(SDL_SaveBMP(surface, path) == 0);
    theme.grid = true;
    for (i = 0U; i < 6U; ++i) {
        char art[512];
        (void)snprintf(art, sizeof(art), "/system/share/console/box-art/%s/Imgs/%s.png",
                      i == 5U ? "GB" : "GBC", titles[i]);
        view.grid_artwork[i] = cl_image_read(art);
        assert(view.grid_artwork[i] != NULL);
    }
    {
        struct cl_art_worker *worker = cl_art_start();
        SDL_Surface *images[6] = {0};
        const char *paths[6];
        bool ready = false;
        assert(worker != NULL);
        for (i = 0U; i < 6U; ++i) paths[i] = "/missing/corrupt.png";
        assert(cl_art_request_grid(worker, paths));
        for (i = 0U; i < 6U; ++i) paths[i] = "/system/share/console/box-art/GBC/Imgs/Pokemon - Crystal.png";
        assert(cl_art_request_grid(worker, paths));
        for (i = 0U; i < 1000U && !ready; ++i) { ready = cl_art_take_grid(worker, images); SDL_Delay(5U); }
        assert(ready);
        for (i = 0U; i < 6U; ++i) assert(images[i] != NULL && images[i]->w == 250);
        for (i = 0U; i < 6U; ++i) paths[i] = "/missing/corrupt.png";
        assert(cl_art_request_grid(worker, paths)); ready = false;
        for (i = 0U; i < 1000U && !ready; ++i) { ready = cl_art_take_grid(worker, images); SDL_Delay(5U); }
        assert(ready);
        for (i = 0U; i < 6U; ++i) assert(images[i] == NULL);
        cl_art_stop(worker);
        puts("ART_GRID_WORKER_PASS stale batch discarded, all six decoded, missing images clear old covers, worker joined");
    }
    cl_view_draw(surface, &view);
    (void)snprintf(path, sizeof(path), "%s/grid.bmp", argv[2]);
    assert(SDL_SaveBMP(surface, path) == 0);
    view.carousel = true;
    history.entries[0].total_play_seconds = 1000000U * 3600U;
    cl_view_draw(surface, &view);
    assert_switcher_logo(surface, &theme, &assets);
    history.entries[0].total_play_seconds = 3892U;
    cl_view_draw(surface, &view);
    assert_switcher_logo(surface, &theme, &assets);
    puts("GAMESWITCHER_LOGO_PIXEL_PASS reserved_x_0_to_179 status_right_aligned_within_180_to_615");
    (void)snprintf(path, sizeof(path), "%s/gameswitcher.bmp", argv[2]);
    assert(SDL_SaveBMP(surface, path) == 0);
    {
        const char *apps[] = {"Package Manager", "Activity Tracker", "Themes", "GameSwitcher", "Refresh ROM library", "Storage recovery", "ScreenScraper", "Package downloads"};
        cl_options_draw(surface, &view, "Apps", apps, 8U, 6U);
        (void)snprintf(path, sizeof(path), "%s/apps.bmp", argv[2]);
        assert(SDL_SaveBMP(surface, path) == 0);
    }
    {
        struct cl_tools tools;
        struct cl_tool_request request;
        struct cl_tool_snapshot snapshot = {0};
        cl_tools_open(&tools, CL_TOOL_SCRAPER, &request);
        snapshot.kind = CL_TOOL_SCRAPER; snapshot.ready = true; snapshot.count = 3U;
        strcpy(snapshot.status, "First-time setup: account and registered API client credentials.");
        strcpy(snapshot.rows[0].id, "@user"); strcpy(snapshot.rows[0].label, "ScreenScraper username");
        strcpy(snapshot.rows[0].detail, "A enters your account name"); snapshot.rows[0].available = true;
        strcpy(snapshot.rows[1].id, "@password"); strcpy(snapshot.rows[1].label, "ScreenScraper password");
        strcpy(snapshot.rows[1].detail, "Saved (hidden)"); snapshot.rows[1].available = true;
        strcpy(snapshot.rows[2].id, "@missing"); strcpy(snapshot.rows[2].label, "Scrape all missing covers"); snapshot.rows[2].available = true;
        assert(cl_tools_accept(&tools, &snapshot)); cl_tools_draw(surface, &view, &tools);
        (void)snprintf(path, sizeof(path), "%s/scraper.bmp", argv[2]); assert(SDL_SaveBMP(surface, path) == 0);
        assert(cl_tools_input(&tools, CL_KEY(CL_A), &request) == CL_TOOL_NO_ACTION);
        assert(tools.password_edit); cl_tools_draw(surface, &view, &tools);
        (void)snprintf(path, sizeof(path), "%s/account-keyboard.bmp", argv[2]); assert(SDL_SaveBMP(surface, path) == 0);
        cl_tools_open(&tools, CL_TOOL_DOWNLOADS, &request);
        snapshot.kind = CL_TOOL_DOWNLOADS; snapshot.count = 2U;
        strcpy(snapshot.status, "Protected AArch64 package SHA256 verification is required.");
        strcpy(snapshot.rows[0].id, "@repository"); strcpy(snapshot.rows[0].label, "Package repository");
        strcpy(snapshot.rows[0].detail, "Set HTTPS URL for native MagicX packages");
        strcpy(snapshot.rows[1].id, "gambatte"); strcpy(snapshot.rows[1].label, "Gambatte");
        strcpy(snapshot.rows[1].detail, "A downloads the protected AArch64 package");
        assert(cl_tools_accept(&tools, &snapshot)); cl_tools_draw(surface, &view, &tools);
        (void)snprintf(path, sizeof(path), "%s/downloads.bmp", argv[2]); assert(SDL_SaveBMP(surface, path) == 0);
    }
    {
        cl_settings_draw(surface, &view, false, 0U);
        (void)snprintf(path, sizeof(path), "%s/settings.bmp", argv[2]);
        assert(SDL_SaveBMP(surface, path) == 0);
        cl_settings_draw(surface, &view, false, 8U);
        (void)snprintf(path, sizeof(path), "%s/settings-lower.bmp", argv[2]);
        assert(SDL_SaveBMP(surface, path) == 0);
    }
    {
        struct cl_tools tools;
        struct cl_tool_request request;
        struct cl_tool_snapshot snapshot = {0};
        cl_tools_open(&tools, CL_TOOL_NETWORK, &request);
        snapshot.kind = CL_TOOL_NETWORK; snapshot.ready = true; snapshot.count = 1U;
        strcpy(snapshot.status, "Wi-Fi: DISCONNECTED | X refreshes status");
        strcpy(snapshot.rows[0].id, "01:02:03:04:05:06");
        strcpy(snapshot.rows[0].label, "Example network");
        strcpy(snapshot.rows[0].detail, "WPA2 password - test fixture only");
        snapshot.rows[0].required = true; snapshot.rows[0].available = true;
        assert(cl_tools_accept(&tools, &snapshot));
        cl_tools_draw(surface, &view, &tools);
        (void)snprintf(path, sizeof(path), "%s/wifi.bmp", argv[2]);
        assert(SDL_SaveBMP(surface, path) == 0);
        assert(cl_tools_input(&tools, CL_KEY(CL_A), &request) == CL_TOOL_NO_ACTION);
        assert(tools.password_edit);
        cl_tools_draw(surface, &view, &tools);
        (void)snprintf(path, sizeof(path), "%s/wifi-password.bmp", argv[2]);
        assert(SDL_SaveBMP(surface, path) == 0);
    }
    cl_view_dialog(surface, &view, "Information", "The selected game is ready. Your in-game saves are retained.", NULL, "OK", 1U);
    (void)snprintf(path,sizeof(path),"%s/dialog.bmp",argv[2]);assert(SDL_SaveBMP(surface,path)==0);
    cl_view_dialog(surface, &view, "Power off", "Save progress and shut down?", "Cancel", "Power off", 0U);
    (void)snprintf(path,sizeof(path),"%s/confirmation.bmp",argv[2]);assert(SDL_SaveBMP(surface,path)==0);
    view.fullscreen = true;view.carousel = true;cl_view_draw(surface,&view);
    (void)snprintf(path,sizeof(path),"%s/switcher-fullscreen.bmp",argv[2]);assert(SDL_SaveBMP(surface,path)==0);
    fprintf(stderr,"SILKY_PHASE_CLOSE\n");fflush(stderr);
    cl_assets_close(&assets);
    SDL_FreeSurface(view.artwork);
    for (i = 0U; i < 6U; ++i) SDL_FreeSurface(view.grid_artwork[i]);
    SDL_FreeSurface(surface);
    SDL_Quit();
    puts("SILKY_RENDER_PASS real PNG tabs/status/system logos + runtime TTF + RGB565 "
         "MainUI/list/grid/GameSwitcher/apps/settings");
    return 0;
}

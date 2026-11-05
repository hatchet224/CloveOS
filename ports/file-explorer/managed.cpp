#include "managed.h"

extern "C" {
#include "launcher/display.h"
#include "launcher/file_explorer.h"
#include "launcher/input.h"
#include "sessiond/store.h"
}

#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "screen.h"

namespace {
cl_display display;
cl_input input;
cs_store card;
volatile std::sig_atomic_t stopping;
uint32_t pending;
bool video_failed;

void stop(int) { stopping = 1; }

SDL_Keycode mappedKey(unsigned control)
{
    const auto &c = config();
    const SDL_Keycode keys[] = {c.key_up, c.key_down, c.key_left, c.key_right,
        c.key_open, c.key_parent, c.key_operation, c.key_select, c.key_pageup,
        c.key_pagedown, c.key_system, SDLK_UNKNOWN, SDLK_UNKNOWN,
        c.key_transfer, c.key_system, c.key_pageup, c.key_pagedown};
    return keys[control];
}
}

bool commanderManagedOpen()
{
    cl_file_explorer_config settings = {};
    const ssize_t size = recv(3, &settings, sizeof(settings), MSG_TRUNC);
    close(3);
    if (size != sizeof(settings) || !memchr(settings.card_uuid, '\0', sizeof(settings.card_uuid)))
        return false;

    cs_store_config storage = {};
    storage.card_path = FILE_SYSTEM;
    storage.card_uuid = settings.card_uuid;
    storage.qualified_fat_syncfs = settings.qualified_fat_syncfs;
    storage.max_state_bytes = 1;
    if (cs_store_open_reader(&card, &storage) != CS_STORE_OK) return false;
    if (!card.fat || faccessat(card.card_fd, ".", W_OK, 0) != 0 ||
        !cl_display_open(&display, &settings.display)) {
        cs_store_close(&card);
        return false;
    }
    if (!cl_input_open(&input, &settings.input)) {
        cl_display_close(&display);
        cs_store_close(&card);
        return false;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    screen.ppu_x = config().disp_ppu_x;
    screen.ppu_y = config().disp_ppu_y;
    screen.setPhysicalResolution(CL_WIDTH, CL_HEIGHT);
    screen.surface = display.surface;
    return true;
}

bool commanderManagedClose()
{
    cl_input_close(&input);
    cl_display_close(&display);
    const bool saved = cs_store_sync(&card) == CS_STORE_OK;
    cs_store_close(&card);
    return saved && !video_failed;
}

bool commanderMediaReady()
{
    return !stopping && cs_store_check_media(&card) == CS_STORE_OK;
}

bool commanderPathAllowed(const std::string &path)
{
    const std::string root = FILE_SYSTEM;
    if (path == root) return true;
    if (path.compare(0, root.size() + 1, root + "/") != 0) return false;
    const size_t first = root.size() + 1;
    if (first == path.size() || path[first] == '.') return false;
    size_t position = first;
    while (position < path.size()) {
        const size_t end = path.find('/', position);
        const std::string part = path.substr(position, end - position);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        position = end + 1;
    }
    const std::string top = path.substr(first, path.find('/', first) - first);
    struct stat target;
    if (fstatat(card.card_fd, top.c_str(), &target, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (target.st_dev != card.device || S_ISLNK(target.st_mode)) return false;
    for (const char *name : {".console", ".tmp_update"}) {
        struct stat protected_dir;
        if (fstatat(card.card_fd, name, &protected_dir, AT_SYMLINK_NOFOLLOW) == 0 &&
            target.st_ino == protected_dir.st_ino) return false;
    }
    return true;
}

bool commanderValidName(const std::string &name)
{
    return !name.empty() && name.front() != '.' && name.back() != '.' && name.back() != ' ' &&
           name.find_first_of("/\\:\"*?<>|") == std::string::npos &&
           name.find('\0') == std::string::npos;
}

bool commanderPollEvent(SDL_Event &event)
{
    const uint64_t now = SDL_GetTicks64();
    if (stopping || video_failed) {
        event = {};
        event.type = SDL_QUIT;
        return true;
    }
    for (;;) {
        while (pending != 0U) {
            const unsigned control = __builtin_ctz(pending);
            pending &= pending - 1U;
            const auto key = mappedKey(control);
            if (key == SDLK_UNKNOWN) continue;
            event = {};
            event.type = SDL_KEYDOWN;
            event.key.keysym.sym = key;
            return true;
        }
        if (!SDL_PollEvent(&event)) {
            pending = cl_input_take(&input, now);
            if (pending == 0U) return false;
            continue;
        }
        cl_input_event(&input, &event, now);
        pending = cl_input_take(&input, now);
        if (event.type == SDL_WINDOWEVENT || event.type == SDL_QUIT) return true;
    }
}

bool commanderPresent()
{
    video_failed = !cl_display_present(&display);
    return !video_failed;
}

#include <SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { A = 0, B = 1, X = 2, UP = 13, LEFT = 14, RIGHT = 15, DOWN = 16, MENU = 19, START = 8 };
static int role;
static uint64_t present_at, last;
static unsigned edge;
static SDL_JoystickID instance = -1;

__attribute__((constructor)) static void test_environment(void)
{
    const char *name = getenv("CLOVE_TEST_ROLE");
    role = name && strcmp(name, "explorer") == 0 ? 2 : name && strcmp(name, "return") == 0 ? 3 : 1;
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
}

int SDL_InitSubSystem(Uint32 flags)
{
    int (*real)(Uint32) = dlsym(RTLD_NEXT, "SDL_InitSubSystem");
    int result = real(flags);
    if (result == 0 && (flags & SDL_INIT_JOYSTICK) && instance < 0) {
        const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_UNKNOWN, 4, 20, 1);
        if (device >= 0) instance = SDL_JoystickGetDeviceInstanceID(device);
    }
    return result;
}

SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int index)
{
    SDL_JoystickGUID (*real)(int) = dlsym(RTLD_NEXT, "SDL_JoystickGetDeviceGUID");
    if (instance >= 0 && SDL_JoystickGetDeviceInstanceID(index) == instance)
        return SDL_JoystickGetGUIDFromString("1234567890abcdef1234567890abcdef");
    return real(index);
}

void SDL_RenderPresent(SDL_Renderer *renderer)
{
    void (*real)(SDL_Renderer *) = dlsym(RTLD_NEXT, "SDL_RenderPresent");
    real(renderer);
    if (present_at == 0) {
        present_at = SDL_GetTicks64();
        if (role == 3) {
            int fd = open("/tmp/mainui-returned", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (fd >= 0) close(fd);
        }
    }
}

static int controller_edge(SDL_Event *event)
{
    const uint64_t now = SDL_GetTicks64();
    static const uint8_t launch[] = {A, UP, UP, A};
    static const uint8_t explorer[] = {
        A, DOWN, A, DOWN, RIGHT, DOWN, DOWN, A, LEFT, X, A, /* copy */
        RIGHT, DOWN, X, DOWN, DOWN, A, B, START,          /* rename */
        X, DOWN, A,                                       /* move */
        LEFT, UP, DOWN, X, DOWN, DOWN, DOWN, A, A,         /* delete */
        MENU, DOWN, DOWN, A, DOWN, A, START,              /* mkdir */
        MENU, DOWN, DOWN, DOWN, DOWN, A,                    /* quit */
    };
    const uint8_t *keys = role == 2 ? explorer : launch;
    const unsigned count = role == 2 ? sizeof(explorer) : sizeof(launch);
    if (role == 3 || instance < 0 || !event || !present_at || now < present_at + 700 ||
        now < last + 145 || edge / 2 >= count) return 0;
    if (role == 2 && edge % 2 == 0) {
        const unsigned step = edge / 2;
        bool ok = true;
        const char *action = NULL;
        if (step == 11) {
            action = "copy";
            ok = access("/mnt/SDCARD/Right/first.txt", F_OK) == 0;
        } else if (step == 19) {
            action = "rename";
            ok = access("/mnt/SDCARD/Right/first.tx", F_OK) == 0 &&
                 access("/mnt/SDCARD/Right/first.txt", F_OK) != 0;
        } else if (step == 22) {
            action = "move";
            ok = access("/mnt/SDCARD/Left/first.tx", F_OK) == 0 &&
                 access("/mnt/SDCARD/Right/first.tx", F_OK) != 0;
        } else if (step == 31) {
            action = "delete";
            ok = access("/mnt/SDCARD/Left/first.tx", F_OK) != 0;
        } else if (step == 38) {
            action = "mkdir";
            ok = access("/mnt/SDCARD/Left/1", F_OK) == 0;
        }
        if (action) fprintf(stderr, "FILE_EXPLORER_CONTROLLER_ACTION %s %s\n", action, ok ? "PASS" : "FAIL");
        if (!ok) _exit(90);
    }
    *event = (SDL_Event){0};
    event->type = edge % 2 == 0 ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    event->jbutton.which = instance;
    event->jbutton.button = keys[edge / 2];
    event->jbutton.state = edge % 2 == 0 ? SDL_PRESSED : SDL_RELEASED;
    ++edge;
    last = now;
    if (role == 2 && edge % 2 != 0 && edge / 2 >= 31)
        fprintf(stderr, "explorer fixture step=%u button=%u time_ms=%llu\n", edge / 2,
                event->jbutton.button, (unsigned long long)(now - present_at));
    return 1;
}

int SDL_PollEvent(SDL_Event *event)
{
    int (*real)(SDL_Event *) = dlsym(RTLD_NEXT, "SDL_PollEvent");
    return real(event) || controller_edge(event);
}

int SDL_WaitEventTimeout(SDL_Event *event, int timeout)
{
    int (*real)(SDL_Event *, int) = dlsym(RTLD_NEXT, "SDL_WaitEventTimeout");
    return controller_edge(event) || real(event, timeout);
}

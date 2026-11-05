#ifdef NDEBUG
#error "raw SDL regression requires assertions"
#endif
#define SDL_MAIN_HANDLED 1
#include <SDL.h>
#include <libretro.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define HAVE_SDL2 1
#define HAVE_MAGICX_BRIDGE 1
#define MAX_USERS 2
#define RARCH_LOG(...) ((void)0)
#define RARCH_WARN(...) ((void)0)
#define RARCH_ERR(...) ((void)0)
#define HAT_UP_MASK (1U<<15)
#define HAT_DOWN_MASK (1U<<14)
#define HAT_LEFT_MASK (1U<<13)
#define HAT_RIGHT_MASK (1U<<12)
#define HAT_MASK (HAT_UP_MASK|HAT_DOWN_MASK|HAT_LEFT_MASK|HAT_RIGHT_MASK)
#define GET_HAT_DIR(x) ((x)&HAT_MASK)
#define GET_HAT(x) ((x)&0xfffU)
#define NO_BTN 0xffffU
#define AXIS_NONE 0xffffffffU
#define AXIS_NEG_GET(x) (((x)>>16)&0xffffU)
#define AXIS_POS_GET(x) ((x)&0xffffU)
#define RARCH_FIRST_CUSTOM_BIND 16
struct retro_keybind { uint64_t joykey;uint32_t joyaxis; };
typedef struct {uint16_t joy_idx;const struct retro_keybind *auto_binds;float axis_threshold;} rarch_joypad_info_t;
static struct {const char *ident;} sdl_joypad={"sdl2"};
struct _SDL_Joystick {int unused;};struct _SDL_GameController {int unused;};
static struct _SDL_Joystick raw_handle;static struct _SDL_GameController controller_handle;
static bool managed;static unsigned raw_opens,controller_opens,raw_reads,controller_reads;
static Uint8 raw_buttons[22],controller_buttons[SDL_CONTROLLER_BUTTON_MAX];
static Sint16 raw_axes[4];
static bool magicx_retroarch_requested(void){return managed;}
static void input_autoconfigure_connect(const char *n,const char *d,const char *i,unsigned id,int32_t v,int32_t p){(void)n;(void)d;(void)i;(void)id;(void)v;(void)p;}
SDL_bool SDL_IsGameController(int id){assert(id==0);return SDL_TRUE;}
SDL_GameController *SDL_GameControllerOpen(int id){assert(id==0);++controller_opens;return &controller_handle;}
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c){assert(c==&controller_handle);return &raw_handle;}
SDL_Joystick *SDL_JoystickOpen(int id){assert(id==0);++raw_opens;return &raw_handle;}
void SDL_JoystickClose(SDL_Joystick *j){assert(j==&raw_handle);}
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *j){SDL_JoystickGUID g={{0}};assert(j==&raw_handle);return g;}
const char *SDL_GameControllerNameForIndex(int id){assert(id==0);return "semantic controller";}
const char *SDL_JoystickNameForIndex(int id){assert(id==0);return "magicx raw22";}
const char *SDL_GetError(void){return "mock SDL";}
int SDL_JoystickNumAxes(SDL_Joystick *j){assert(j==&raw_handle);return 4;}
int SDL_JoystickNumButtons(SDL_Joystick *j){assert(j==&raw_handle);return 22;}
int SDL_JoystickNumHats(SDL_Joystick *j){assert(j==&raw_handle);return 0;}
int SDL_JoystickNumBalls(SDL_Joystick *j){assert(j==&raw_handle);return 0;}
Uint8 SDL_JoystickGetButton(SDL_Joystick *j,int b){assert(j==&raw_handle);assert(b>=0&&b<22);++raw_reads;return raw_buttons[b];}
Uint8 SDL_GameControllerGetButton(SDL_GameController *c,SDL_GameControllerButton b){assert(c==&controller_handle);assert(b>=0&&b<SDL_CONTROLLER_BUTTON_MAX);++controller_reads;return controller_buttons[b];}
Uint8 SDL_JoystickGetHat(SDL_Joystick *j,int h){(void)j;(void)h;return 0;}
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j,int a){assert(j==&raw_handle);assert(a>=0&&a<4);return raw_axes[a];}
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c,SDL_GameControllerAxis a){(void)c;(void)a;return 0;}
SDL_Haptic *SDL_HapticOpenFromJoystick(SDL_Joystick *j){(void)j;return NULL;}
int SDL_HapticEffectSupported(SDL_Haptic *h,SDL_HapticEffect *e){(void)h;(void)e;return 0;}

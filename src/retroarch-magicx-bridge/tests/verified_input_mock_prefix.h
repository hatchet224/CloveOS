#ifdef NDEBUG
#error "Verified input fixture requires active assertions (-UNDEBUG)"
#endif
#define _GNU_SOURCE
#define HAVE_SDL2 1
#include "magicx_bridge.h"
#include "launcher/qualification.h"
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include <SDL.h>
#include <libretro.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define AXIS_NONE 0xFFFFFFFFUL
#define NO_BTN 0xFFFFU
#define AXIS_POS(x) (0xffff0000UL | (unsigned long)(x))
#define AXIS_NEG(x) (0xffffUL | ((unsigned long)(x) << 16))
#define RARCH_ANALOG_LEFT_X_PLUS 32U
#define HAT_UP_MASK (1U << 15)
#define HAT_DOWN_MASK (1U << 14)
#define HAT_LEFT_MASK (1U << 13)
#define HAT_RIGHT_MASK (1U << 12)
#define HAT_MAP(x,hat) (((x) & ((1U << 12)-1U)) | (hat))
enum { RARCH_ENABLE_HOTKEY=20,RARCH_FAST_FORWARD_KEY,RARCH_SLOWMOTION_KEY,
       RARCH_STATE_SLOT_MINUS,RARCH_STATE_SLOT_PLUS,RARCH_SCREENSHOT,RARCH_QUIT_KEY };
/* Minimal RA host storage; meta bind indices are symbolic fixtures, not an ABI
 * claim. The production function itself is extracted without modification. */
struct host_bind { uint16_t joykey; unsigned long joyaxis; };
static struct host_bind input_config_binds[1][64];
struct host_settings { struct {unsigned input_joypad_index[16];} uints; };
static struct host_settings settings;
static struct host_settings *config_get_ptr(void) { return &settings; }
struct _SDL_Joystick { int index; };
static struct _SDL_Joystick joy;
static int device_count=1,matching_index=0,buttons=22,hats=0,init_fails,open_fails,all_match;
static int axes=4;
static unsigned int init_calls,quit_calls,close_calls;
static Uint32 initialized;
static const char *expected_guid="1900385212b400006666000000010000";
Uint32 SDL_WasInit(Uint32 flags) { return initialized & flags; }
int SDL_InitSubSystem(Uint32 flags) { ++init_calls;if(init_fails)return -1;initialized|=flags;return 0; }
void SDL_QuitSubSystem(Uint32 flags) { ++quit_calls;initialized&=~flags; }
int SDL_NumJoysticks(void) { return device_count; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int index) {
 SDL_JoystickGUID g;size_t i;const char *s=(index==matching_index||all_match)?expected_guid:"f900385212b400006666000000010000";
 for(i=0;i<16;++i){unsigned int b;assert(sscanf(s+2*i,"%2x",&b)==1);g.data[i]=(Uint8)b;}return g;
}
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid,char *text,int size) {
 size_t i;assert(size>=33);for(i=0;i<16;++i)assert(snprintf(text+2*i,3,"%02x",guid.data[i])==2);
}
SDL_Joystick *SDL_JoystickOpen(int index) { if(open_fails)return NULL;joy.index=index;return &joy; }
int SDL_JoystickNumButtons(SDL_Joystick *selected) { assert(selected==&joy);return buttons; }
int SDL_JoystickNumHats(SDL_Joystick *selected) { assert(selected==&joy);return hats; }
int SDL_JoystickNumAxes(SDL_Joystick *selected) { assert(selected==&joy);return axes; }
void SDL_JoystickClose(SDL_Joystick *selected) { assert(selected==&joy);++close_calls; }

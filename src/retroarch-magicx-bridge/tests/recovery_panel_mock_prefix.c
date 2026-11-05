#define _GNU_SOURCE
#ifdef NDEBUG
#error assertions required
#endif
#define HAVE_MENU 1
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include "configuration.h"
#include "menu/menu_driver.h"
#include "magicx_bridge.h"
#include "magicx_controls.h"
static struct {
 bool managed,quick_menu,recovery_panel,content_closed;
 unsigned int recovery_confirm;uint32_t recovery_shown_flags;
 struct magicx_control_feedback feedback;
 struct magicx_controls controls;uint64_t next_status;
} ra;
static settings_t settings;
static bool paused,ctl_ok=true;
static struct menu_state menu;
static menu_handle_t menu_data;
static char socket_path[108];
static unsigned int op,opened,closed,notifications;
struct menu_state *menu_state_get_ptr(void) {return &menu;}
settings_t *config_get_ptr(void) {return &settings;}
static bool ra_paused(void *x) {(void)x;return paused;}
static bool ra_set_paused(void *x,bool value) {(void)x;paused=value;return true;}
static bool error_text(char *out,const char *text) {strcpy(out,text);return false;}
static const char *control_path(void) {return socket_path;}
static uint64_t controls_now(void) {return 1000U;}
static void feedback_refresh(const char *x) {(void)x;++notifications;}
static void control_request(unsigned int operation) {op=operation;}
void retroarch_menu_running(void) {++opened;menu.flags|=MENU_ST_FLAG_ALIVE;}
void retroarch_menu_running_finished(bool quit) {(void)quit;++closed;paused=false;menu.flags&=~MENU_ST_FLAG_ALIVE;}
bool menu_driver_ctl(enum rarch_menu_ctl_state state,void *data) {(void)data;assert(state==RARCH_MENU_CTL_SET_PENDING_QUICK_MENU);return ctl_ok;}

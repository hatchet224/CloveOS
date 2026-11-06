#ifdef NDEBUG
#undef NDEBUG
#endif
int supervisor_program_main(int argc, char **argv);
#define sc_spawn test_spawn
#define sc_now test_now
#define sc_identity_file test_identity
#define sc_term_once test_term
#define main supervisor_program_main
#include "../src/main.c"
#undef main
#undef sc_spawn
#undef sc_now
#undef sc_identity_file
#undef sc_term_once
#include <assert.h>
static unsigned int spawned;
static char target_kind[16], target_step[16];
static uint64_t test_clock;
bool test_identity(const char *path,const struct cs_identity *expected)
{(void)path;(void)expected;return true;}
bool test_term(pid_t pid,bool *sent)
{(void)pid;(void)sent;return true;}
bool test_now(uint64_t *now)
{
    *now = test_clock;
    return true;
}
bool test_spawn(const char *path, char *const argv[], int control, int hook,
                 const char *bridge_launch, pid_t *pid)
{
    assert(strcmp(path, "/system/bin/console-level") == 0);
    assert(strcmp(argv[0], path) == 0);
    if (strcmp(argv[1], "restore") == 0) {
        assert(strcmp(argv[2],"/card")==0 && strcmp(argv[3],"test-card")==0 && argv[4]==NULL);
        strcpy(target_kind,"restore");target_step[0]='\0';++spawned;*pid=999;return true;
    }
    assert(argv[5] == NULL && strcmp(argv[3], "/card") == 0 && strcmp(argv[4], "test-card") == 0);
    assert(control == -1 && hook == -1 && bridge_launch == NULL);
    assert(strlen(argv[1]) < sizeof(target_kind) && strlen(argv[2]) < sizeof(target_step));
    strcpy(target_kind, argv[1]);
    strcpy(target_step, argv[2]);
    ++spawned;
    *pid = 999;
    return true;
}
int main(void)
{
    struct supervisor *s = calloc(1U, sizeof(*s));
    struct sc_level level = {false, 1};
    assert(s != NULL);
    s->ui_fd = -1;
    strcpy(s->config.level_helper, "/system/bin/console-level");
    strcpy(s->config.base.card_path, "/card");
    strcpy(s->config.base.card_uuid, "test-card");
    s->status.state = CS_GAME_RUNNING;
    s->status.active = true;
    s->ra_pid = 123;
    level_request(s, &level);
    assert(spawned == 1U && s->level_pid == 999);
    assert(strcmp(target_kind, "volume") == 0 && strcmp(target_step, "up") == 0);
    level_request(s, &level);
    assert(spawned == 1U); /* A tracked helper owns the only auxiliary slot. */
    s->level_pid = 0;
    s->status.unsettled = true;
    level_request(s, &level);
    assert(spawned == 1U);
    s->status.unsettled = false;
    s->stopping = true;
    level_request(s, &level);
    assert(spawned == 1U);
    s->stopping = false;
    s->hook_pending = true;
    level_request(s, &level);
    assert(spawned == 1U);
    s->hook_pending = false;
    s->status.state = CS_CHECKPOINT_WRITING;
    level_request(s, &level);
    assert(spawned == 1U);
    s->status.state = CS_GAME_RUNNING;
    s->ra_pid = 0;
    level_request(s, &level);
    assert(spawned == 1U);
    s->status.state = CS_MENU_READY;
    s->status.active = false;
    level.brightness = true;
    level.direction = -1;
    level_request(s, &level);
    assert(spawned == 2U && s->level_pid == 999);
    assert(strcmp(target_kind, "brightness") == 0 && strcmp(target_step, "down") == 0);
    s->level_pid = 0;
    level.direction = 2;
    level_request(s, &level);
    assert(spawned == 2U);
    s->status.state = CS_QUICK_MENU;
    s->status.active = true;
    s->ra_pid = 123;
    level.direction = 1;
    level_request(s, &level);
    assert(spawned == 3U && s->level_pid == 999);
    s->level_pid = 0;
    s->status.unsettled = true;
    level_request(s, &level);
    assert(spawned == 3U);
    s->status.unsettled = false;
    /* Exercise the actual raw evdev volume path with a pipe. No SDL launcher
     * exists during native Quick Menu, so these physical events own levels. */
    {
        int fds[2];
        struct input_event ev = {0};
        assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
        cs_evdev_init(&s->menu); s->menu.fd = fds[0]; s->menu.code = KEY_BACK;
        s->boot_checked = true;
        ev.time.tv_sec = 1; ev.type = EV_KEY; ev.code = KEY_VOLUMEUP; ev.value = 1;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 1000U; menu_input(s, 1000U);
        assert(spawned == 4U && strcmp(target_kind, "volume") == 0);
        s->level_pid = 0;
        cs_hotkeys_menu(&s->hotkeys, true, false, 1100U);
        ev.time.tv_sec = 1; ev.time.tv_usec = 200000; ev.code = KEY_VOLUMEDOWN;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 1200U; menu_input(s, 1200U);
        assert(spawned == 5U && strcmp(target_kind, "brightness") == 0);
        cs_hotkeys_menu(&s->hotkeys, false, false, 1300U);
        assert(cs_hotkeys_tick(&s->hotkeys, 1500U) == CS_MENU_NONE);
        /* A MENU edge arriving after the caller samples time must survive
         * the subsequent read and modify the accompanying volume-up edge. */
        s->level_pid = 0;
        cs_hotkeys_reset(&s->hotkeys, false);
        ev.time.tv_sec = 2; ev.time.tv_usec = 1000; ev.code = KEY_BACK; ev.value = 1;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        ev.code = KEY_VOLUMEUP;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 2002U; menu_input(s, 2000U);
        assert(spawned == 6U && strcmp(target_kind, "brightness") == 0 &&
               strcmp(target_step, "up") == 0);
        s->level_pid = 0;
        cs_hotkeys_reset(&s->hotkeys, false);
        ev.time.tv_sec = 3; ev.time.tv_usec = 0; ev.code = KEY_BACK;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 2003U; menu_input(s, 2002U);
        assert(!s->hotkeys.down && spawned == 6U); /* Real future edge rejected. */
        s->menu.opened_ms = 2100U;
        ev.time.tv_sec = 2; ev.time.tv_usec = 50000;
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 2200U; menu_input(s, 2199U);
        assert(!s->hotkeys.down && spawned == 6U); /* Pre-open stale edge rejected. */
        assert(close(fds[1]) == 0); cs_evdev_close(&s->menu);
    }
    s->status.state=CS_MENU_READY;s->status.active=false;s->level_pid=0;
    level.direction=0;level_request(s,&level);
    assert(spawned==7U && strcmp(target_kind,"restore")==0);
    s->level_pid=0;s->selected=&s->config.games[0];s->ra_pid=123;
    char published[]="/tmp/clove-level-XXXXXX.identity.json";
    int identity_fd=mkstemps(published,14);assert(identity_fd>=0&&close(identity_fd)==0);
    assert(snprintf(s->config.base.bridge,sizeof(s->config.base.bridge),"%.*s",
                    (int)strlen(published)-14,published)>0);
    s->hook_pending=true;s->hook.request.operation=CS_OP_START;s->hook_token=55;
    service_hook(s,10000U);
    assert(spawned==8U && s->start_levels_restore && s->hook_pending && s->hook_out.count==0U);
    service_hook(s,10001U);assert(spawned==8U && s->hook_pending);
    s->level_pid=0;assert(finish_start_levels(s,0) && !s->hook_pending && s->hook_out.count==1U);
    sc_discard(&s->hook_out);
    s->start_levels_restore=true;s->hook_pending=true;s->ra_pid=0;
    assert(finish_start_levels(s,0) && !s->hook_pending && s->hook_out.count==1U);
    assert(!finish_start_levels(s,0));sc_discard(&s->hook_out);
    assert(unlink(published)==0);
    puts("LEVEL_RESTORE_LIFECYCLE_PASS UI-ready restore; START waits for helper; exited core never acknowledged ready");
    {
        int fds[2];
        assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
        cs_evdev_init(&s->menu); s->menu.fd = fds[0]; s->menu.code = KEY_BACK;
        s->status.state = CS_GAME_RUNNING; s->status.active = true; s->ra_pid = 123;
        s->preferences = UINT64_C(1) << CL_BRIGHTNESS_KEY_SHIFT;
        cs_hotkeys_reset(&s->hotkeys, false);
        cs_hotkeys_menu(&s->hotkeys, true, false, 11000U);
        struct input_event ev = {.time = {.tv_sec = 11, .tv_usec = 1000},
                                 .type = EV_KEY, .code = BTN_DPAD_UP, .value = 1};
        assert(write(fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
        test_clock = 11002U; menu_input(s, test_clock);
        assert(spawned == 9U && strcmp(target_kind, "brightness") == 0 &&
               strcmp(target_step, "down") == 0);
        cs_hotkeys_menu(&s->hotkeys, false, false, 11003U);
        assert(cs_hotkeys_tick(&s->hotkeys, 11500U) == CS_MENU_NONE);
        assert(close(fds[1]) == 0); cs_evdev_close(&s->menu);
        puts("IN_GAME_DPAD_BRIGHTNESS_PASS");
    }
    s->status.state=CS_MENU_READY;s->status.active=false;s->level_pid=0;
    s->hook_pending=true;level.direction=0;level_request(s,&level);
    assert(s->display_levels_pending&&spawned==9U);
    s->hook_pending=false;s->standby_active=true;service_display_levels(s);
    assert(s->display_levels_pending&&spawned==9U);
    s->standby_active=false;s->level_pid=321;service_display_levels(s);
    assert(s->display_levels_pending&&spawned==9U);
    s->level_pid=0;service_display_levels(s);
    assert(!s->display_levels_pending&&spawned==10U&&strcmp(target_kind,"restore")==0);
    service_display_levels(s);assert(spawned==10U);
    puts("WAKE_UI_READY_REPLAY_QUEUE_PASS hook_and_helper_busy_do_not_drop_restore=1");
    s->level_pid=0;s->status.state=CS_STANDBY;s->status.active=true;s->ra_pid=123;
    test_clock=12000U;queue_wake_levels(s);
    assert(s->wake_levels_pending&&s->wake_levels_after==12075U);
    test_clock=12100U;service_display_levels(s);assert(spawned==10U);
    s->status.state=CS_GAME_RUNNING;test_clock=12050U;
    service_display_levels(s);assert(spawned==10U);
    test_clock=12100U;service_display_levels(s);
    assert(spawned==11U&&!s->wake_levels_pending&&!s->display_levels_pending);
    puts("WAKE_GAME_RELEASE_REPLAY_PASS waits_for_running_state_and_display_settle=1");
    free(s);
    puts("LEVEL_ROUTER_PASS running-game/native-Quick-Menu/menu allowed; actual raw volume path and brightness chord consumed; checkpoint/shutdown/aux barriers preserved");
    puts("LEVEL_CLOCK_RACE_PASS post-read MENU edge retained; brightness-up routed; future and stale edges rejected");
    return 0;
}

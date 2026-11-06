#include "supervisor/process.h"
#include "launcher/file_explorer.h"
#include "launcher/ports.h"
#include "launcher/preferences.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
static bool spawn(const char *path, char *const argv[], int control, int hook,
                  const char *bridge_launch, char *game_settings, pid_t *pid)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t empty, defaults;
    int saved_control = -1, saved_hook = -1, result;
    char **environment = NULL;
    char setting[1024];
    size_t count = 0U, i, used = 0U;
    bool actions_ready = false, attributes_ready = false, ok = false;
    if (control >= 0 && (saved_control = fcntl(control, F_DUPFD_CLOEXEC, 10)) < 0) {
        goto done;
    }
    if (hook >= 0 && (saved_hook = fcntl(hook, F_DUPFD_CLOEXEC, 10)) < 0) {
        goto done;
    }
    while (environ[count] != NULL) {
        ++count;
    }
    environment = calloc(count + 4U, sizeof(*environment));
    if (environment == NULL) {
        goto done;
    }
    for (i = 0U; i < count; ++i) {
        if (strncmp(environ[i], "MAGICX_BRIDGE_LAUNCH=", 21U) != 0 &&
            strncmp(environ[i], "MAGICX_INPUT_PROFILE=", 21U) != 0 &&
            strncmp(environ[i], "MAGICX_GAME_TWEAKS=", sizeof("MAGICX_GAME_TWEAKS=") - 1U) != 0) {
            environment[used++] = environ[i];
        }
    }
    if (bridge_launch != NULL) {
        result = snprintf(setting, sizeof(setting), "MAGICX_BRIDGE_LAUNCH=%s", bridge_launch);
        if (result < 0 || (size_t)result >= sizeof(setting)) {
            goto done;
        }
        environment[used++] = setting;
        environment[used++] = "MAGICX_INPUT_PROFILE=/run/console/config/verified-input.json";
    }
    if (game_settings != NULL) environment[used++] = game_settings;
    environment[used] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        goto done;
    }
    actions_ready = true;
    if (posix_spawnattr_init(&attributes) != 0) {
        goto done;
    }
    attributes_ready = true;
    if ((saved_control >= 0 && posix_spawn_file_actions_adddup2(&actions, saved_control, 3) != 0) ||
        (saved_hook >= 0 && posix_spawn_file_actions_adddup2(&actions, saved_hook, 4) != 0) ||
        sigemptyset(&empty) != 0 || sigemptyset(&defaults) != 0 ||
        sigaddset(&defaults, SIGTERM) != 0 || sigaddset(&defaults, SIGINT) != 0 ||
        sigaddset(&defaults, SIGPIPE) != 0 || sigaddset(&defaults, SIGCHLD) != 0 ||
        posix_spawnattr_setsigmask(&attributes, &empty) != 0 ||
        posix_spawnattr_setsigdefault(&attributes, &defaults) != 0 ||
        posix_spawnattr_setpgroup(&attributes, 0) != 0 ||
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                                  POSIX_SPAWN_SETPGROUP) != 0) {
        goto done;
    }
    result = posix_spawn(pid, path, &actions, &attributes, argv, environment);
    if (result != 0) {
        errno = result;
        goto done;
    }
    ok = true;
done:
    if (actions_ready) {
        (void)posix_spawn_file_actions_destroy(&actions);
    }
    if (attributes_ready) {
        (void)posix_spawnattr_destroy(&attributes);
    }
    if (saved_control >= 0) {
        (void)close(saved_control);
    }
    if (saved_hook >= 0) {
        (void)close(saved_hook);
    }
    free(environment);
    return ok;
}
bool sc_spawn(const char *path, char *const argv[], int control, int hook,
              const char *bridge_launch, pid_t *pid)
{
    return spawn(path, argv, control, hook, bridge_launch, NULL, pid);
}

bool sc_spawn_game(const char *path, char *const argv[], const char *bridge_launch,
                   uint64_t appearance, uint32_t shortcuts, pid_t *pid)
{
    char settings[64];
    unsigned int rate = cl_setting_value(shortcuts, CL_FAST_FORWARD_SHIFT, 6U);
    int n = snprintf(settings, sizeof(settings), "MAGICX_GAME_TWEAKS=%u,%u,%u,%u",
                     cl_setting_value(shortcuts, CL_TRIGGER_SWAP_SHIFT, 1U),
                     rate <= CL_FAST_FORWARD_MAX ? rate : 0U,
                     cl_setting_value(appearance, CL_OSD_SHIFT, 4U),
                     (appearance & CL_HIDE_HINT_LABELS) != 0U ? 1U : 0U);
    return n > 0 && (size_t)n < sizeof(settings) &&
           spawn(path, argv, -1, -1, bridge_launch, settings, pid);
}
bool sc_spawn_file_explorer(const struct cl_file_explorer_config *config, pid_t *pid)
{
    int pair[2];
    char program[] = CL_FILE_EXPLORER_PATH;
    char *args[] = {program, NULL};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) return false;
    bool started = send(pair[0], config, sizeof(*config), MSG_NOSIGNAL) == (ssize_t)sizeof(*config) &&
                   sc_spawn(program, args, pair[1], -1, NULL, pid);
    close(pair[0]);
    close(pair[1]);
    return started;
}
bool sc_spawn_portmaster(const struct cl_ports_config *config, pid_t *pid)
{
    int pair[2];char program[]="/system/bin/console-ports";char *args[]={program,NULL};
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)!=0)return false;
    bool ok=send(pair[0],config,sizeof(*config),MSG_NOSIGNAL)==(ssize_t)sizeof(*config) &&
            sc_spawn(program,args,pair[1],-1,NULL,pid);
    close(pair[0]);close(pair[1]);return ok;
}
bool sc_term_once(pid_t pid, bool *sent)
{
    if (pid <= 0 || *sent) {
        return true;
    }
    /* Only tracked, unreaped direct children: their PIDs cannot be reused yet.
     * A second RetroArch SIGTERM invokes its emergency exit path, so never repeat. */
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
    }
    *sent = true;
    return true;
}
bool sc_shutdown_record(bool create)
{
    char boot[64], actual[64];
    int source = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    int directory = -1, fd = -1;
    ssize_t n, got;
    bool ok = false;
    struct stat st;
    if (source < 0) {
        return false;
    }
    do {
        n = read(source, boot, sizeof(boot));
    } while (n < 0 && errno == EINTR);
    if (close(source) != 0 || n <= 0 || (size_t)n >= sizeof(boot)) {
        return false;
    }
    directory = open("/run/console", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
        return false;
    }
    if (create) {
        fd = openat(directory, "shutdown.ready.tmp",
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            do {
                got = write(fd, boot, (size_t)n);
            } while (got < 0 && errno == EINTR);
            ok = got == n && fsync(fd) == 0;
            if (close(fd) != 0) {
                ok = false;
            }
            fd = -1;
            if (ok) {
                ok = renameat(directory, "shutdown.ready.tmp", directory, "shutdown.ready") == 0 &&
                     fsync(directory) == 0;
            }
        }
    } else {
        fd = openat(directory, "shutdown.ready", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
            st.st_uid == geteuid() && (st.st_mode & 0077U) == 0U && st.st_size == n) {
            do {
                got = read(fd, actual, sizeof(actual));
            } while (got < 0 && errno == EINTR);
            ok = got == n && memcmp(actual, boot, (size_t)n) == 0;
        }
    }
    if (fd >= 0 && close(fd) != 0) {
        ok = false;
    }
    if (close(directory) != 0) {
        ok = false;
    }
    return ok;
}

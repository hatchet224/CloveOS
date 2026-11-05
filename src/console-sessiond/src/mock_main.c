/* Linux 4.9-compatible harness. Every external effect uses fake_bridge.c.
 * No evdev, PMIC, filesystem commit, reboot, or suspend backend is linked. */
#include "sessiond/commands.h"
#include "sessiond/fake_bridge.h"
#include "sessiond/power_input.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MAX_SCRIPT_COMMANDS 256U
#define MAX_LINE 256U
#define MAX_PUMP_ROUNDS 64U

enum fd_tag { FD_TIMER = 1, FD_SIGNAL, FD_INPUT };

struct scheduled_command {
    uint64_t offset_ms;
    struct cs_command command;
};

struct application {
    struct cs_fsm fsm;
    struct cs_power_input power;
    struct cs_fake_bridge fake;
    struct scheduled_command script[MAX_SCRIPT_COMMANDS];
    size_t script_count;
    size_t script_index;
    uint64_t started_at_ms;
    bool scripted;
    bool running;
    bool failed;
    char input_line[MAX_LINE];
    size_t input_length;
    bool input_overflow;
};

static bool monotonic_ms(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return false;
    }
    *value = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return true;
}

static void trace_transition(void *context, enum cs_state from, enum cs_state to,
                             const char *reason)
{
    struct application *app = context;
    printf("STATE %s -> %s session=%" PRIu64 " generation=%" PRIu64 " reason=%s\n",
           cs_state_name(from), cs_state_name(to), app->fsm.session_id,
           app->fsm.committed_generation, reason);
}

static bool submit_request(void *context, const struct cs_request *request, uint64_t now_ms)
{
    struct application *app = context;
    printf("MOCK_REQUEST %s session=%" PRIu64 " request=%" PRIu64 " generation=%" PRIu64 "\n",
           cs_operation_name(request->operation), request->session_id, request->request_id,
           request->generation);
    return cs_fake_submit(&app->fake, request, now_ms);
}

static bool parse_u64(const char *text, const char **end, uint64_t *value)
{
    char *tail = NULL;
    unsigned long long parsed;
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &tail, 10);
    if (errno == ERANGE || tail == text || parsed != (unsigned long long)(uint64_t)parsed) {
        return false;
    }
    *value = (uint64_t)parsed;
    *end = tail;
    return true;
}

static bool load_script(struct application *app, const char *path)
{
    FILE *file = fopen(path, "r");
    char line[MAX_LINE];
    size_t line_number = 0U;
    bool valid = true;
    if (file == NULL) {
        perror("open script");
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        const char *cursor = line;
        uint64_t offset;
        size_t length = strlen(line);
        struct cs_command command;
        ++line_number;
        if (length == sizeof(line) - 1U && line[length - 1U] != '\n') {
            valid = false;
            break;
        }
        while (length > 0U && (line[length - 1U] == '\r' || line[length - 1U] == '\n')) {
            line[--length] = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        if (app->script_count == MAX_SCRIPT_COMMANDS || !parse_u64(cursor, &cursor, &offset) ||
            *cursor != ' ' ||
            (app->script_count > 0U && offset < app->script[app->script_count - 1U].offset_ms)) {
            valid = false;
            break;
        }
        while (*cursor == ' ') {
            ++cursor;
        }
        if (!cs_command_parse(cursor, &command)) {
            valid = false;
            break;
        }
        app->script[app->script_count].offset_ms = offset;
        app->script[app->script_count++].command = command;
    }
    if (ferror(file) != 0 || app->script_count == 0U) {
        valid = false;
    }
    if (valid && app->script[app->script_count - 1U].command.kind != CS_CMD_EXPECT &&
        app->script[app->script_count - 1U].command.kind != CS_CMD_QUIT) {
        valid = false; /* Never report a test pass before its final assertion. */
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    if (!valid) {
        fprintf(stderr, "Invalid/oversized script near line %zu\n", line_number);
    }
    return valid;
}

static void execute_command(struct application *app, const struct cs_command *command,
                            uint64_t now_ms)
{
    struct cs_event event = {0};
    switch (command->kind) {
    case CS_CMD_EVENT:
        event.kind = command->event;
        if (event.kind == CS_EV_EMULATOR_EXIT) {
            app->fake.core_alive = false;
        }
        cs_fsm_dispatch(&app->fsm, &event, now_ms);
        break;
    case CS_CMD_PRESS:
    case CS_CMD_RELEASE:
    case CS_CMD_REPEAT:
        cs_power_raw(&app->power,
                     command->kind == CS_CMD_PRESS ? 1 : (command->kind == CS_CMD_RELEASE ? 0 : 2),
                     now_ms);
        break;
    case CS_CMD_WAKE:
        if (!cs_fake_wake(&app->fake, now_ms)) {
            fprintf(stderr, "wake ignored: no simulated suspend pending\n");
        }
        break;
    case CS_CMD_FAIL:
        app->fake.fail_next = command->operation;
        break;
    case CS_CMD_DROP:
        app->fake.drop_next = command->operation;
        break;
    case CS_CMD_STATUS:
        printf("STATUS %s session=%" PRIu64 " committed=%" PRIu64 " unsettled=%d\n",
               cs_state_name(app->fsm.state), app->fsm.session_id, app->fsm.committed_generation,
               app->fsm.writer_unsettled ? 1 : 0);
        break;
    case CS_CMD_QUIT:
        app->running = false;
        break;
    case CS_CMD_EXPECT:
        if (app->fsm.state != command->expected_state) {
            fprintf(stderr, "EXPECT failed: wanted %s, got %s\n",
                    cs_state_name(command->expected_state), cs_state_name(app->fsm.state));
            app->failed = true;
            app->running = false;
        } else {
            printf("EXPECT passed: %s\n", cs_state_name(app->fsm.state));
        }
        break;
    }
}

static bool pump_due(struct application *app, uint64_t now_ms)
{
    unsigned int rounds;
    struct cs_event event = {0};
    /* Expired work cannot win a race by delivering success after its deadline. */
    event.kind = CS_EV_TICK;
    cs_fsm_dispatch(&app->fsm, &event, now_ms);
    for (rounds = 0U; rounds < MAX_PUMP_ROUNDS; ++rounds) {
        if (cs_power_poll(&app->power, now_ms, &event)) {
            cs_fsm_dispatch(&app->fsm, &event, now_ms);
            if (event.kind == CS_EV_POWER_DOWN && app->fsm.state == CS_SUSPEND_ENTERING) {
                (void)cs_fake_wake(&app->fake, now_ms);
            }
        } else if (cs_fake_poll(&app->fake, now_ms, &event)) {
            cs_fsm_dispatch(&app->fsm, &event, now_ms);
        } else {
            break;
        }
    }
    /* Work still due is rearmed for the next loop; never spin without epoll. */
    while (app->running && app->script_index < app->script_count &&
           now_ms >=
               cs_deadline_after(app->started_at_ms, app->script[app->script_index].offset_ms)) {
        execute_command(app, &app->script[app->script_index++].command, now_ms);
    }
    if (app->scripted && app->script_index == app->script_count) {
        app->running = false;
    }
    return app->running;
}

static uint64_t next_deadline(const struct application *app)
{
    uint64_t next = app->fsm.deadline_ms;
    const uint64_t power = cs_power_deadline(&app->power);
    if (power < next) {
        next = power;
    }
    if (app->fake.due_ms < next) {
        next = app->fake.due_ms;
    }
    if (app->script_index < app->script_count) {
        const uint64_t script =
            cs_deadline_after(app->started_at_ms, app->script[app->script_index].offset_ms);
        if (script < next) {
            next = script;
        }
    }
    return next;
}

static bool arm_timer(int fd, uint64_t deadline_ms)
{
    struct itimerspec timer;
    memset(&timer, 0, sizeof(timer));
    if (deadline_ms != CS_TIME_NEVER) {
        timer.it_value.tv_sec = (time_t)(deadline_ms / 1000U);
        timer.it_value.tv_nsec = (long)((deadline_ms % 1000U) * 1000000U);
        if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0) {
            timer.it_value.tv_nsec = 1;
        }
    }
    return timerfd_settime(fd, TFD_TIMER_ABSTIME, &timer, NULL) == 0;
}

static bool add_fd(int epoll_fd, int fd, enum fd_tag tag)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u32 = (uint32_t)tag;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static bool drain_timer(int fd)
{
    uint64_t expirations;
    ssize_t result;
    do {
        result = read(fd, &expirations, sizeof(expirations));
    } while (result < 0 && errno == EINTR);
    return result == (ssize_t)sizeof(expirations) || (result < 0 && errno == EAGAIN);
}

static bool consume_input(struct application *app, uint64_t now_ms)
{
    char bytes[256];
    ssize_t count;
    size_t i;
    /* One bounded read per dispatch preserves fairness under a noisy client. */
    do {
        count = read(STDIN_FILENO, bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        return errno == EAGAIN;
    }
    if (count == 0) {
        if (app->input_length != 0U || app->input_overflow) {
            fprintf(stderr, "Input ended with an incomplete command\n");
            return false;
        }
        app->running = false;
        return true;
    }
    for (i = 0U; i < (size_t)count && app->running; ++i) {
        if (bytes[i] == '\n') {
            struct cs_command command;
            if (app->input_length > 0U && app->input_line[app->input_length - 1U] == '\r') {
                --app->input_length;
            }
            app->input_line[app->input_length] = '\0';
            if (app->input_overflow || !cs_command_parse(app->input_line, &command)) {
                fprintf(stderr, "Invalid/oversized mock command\n");
            } else {
                execute_command(app, &command, now_ms);
            }
            app->input_length = 0U;
            app->input_overflow = false;
        } else if (bytes[i] == '\0') {
            app->input_overflow = true;
        } else if (app->input_length + 1U < sizeof(app->input_line)) {
            app->input_line[app->input_length++] = bytes[i];
        } else {
            app->input_overflow = true;
        }
    }
    return true;
}

static int acquire_lock(void)
{
    char path[128];
    struct stat status;
    int fd;
    const int length =
        snprintf(path, sizeof(path), "/tmp/console-sessiond-%lu.lock", (unsigned long)getuid());
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    /* Do not unlink: the inode must remain stable for other lock contenders. */
    return fd;
}

static void usage(const char *program)
{
    printf("Usage: %s [--script FILE] [--mock-suspend] [--mock-resume] [--held-at-boot]\n"
           "       [--delay-ms N]\n"
           "MOCK ONLY: no real power actions, saves, or RetroArch process.\n"
           "Commands: launch resume menu press release repeat wake retry continue\n"
           "          confirm-off recovery-menu critical crash status quit\n"
           "          fail OP / drop OP / expect STATE\n",
           program);
}

int main(int argc, char **argv)
{
    struct application app;
    struct cs_config config = cs_config_default();
    struct cs_ports ports;
    sigset_t signals;
    sigset_t previous_signals;
    struct epoll_event ready[4];
    uint64_t latency_ms = 20U;
    uint64_t now_ms;
    const char *script_path = NULL;
    bool resume_on_boot = false;
    bool held_at_boot = false;
    bool signals_blocked = false;
    int epoll_fd = -1;
    int signal_fd = -1;
    int timer_fd = -1;
    int lock_fd = -1;
    int input_flags = -1;
    int exit_code = EXIT_FAILURE;
    int i;
    memset(&app, 0, sizeof(app));
    (void)setvbuf(stdout, NULL, _IOLBF, 0U);
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--mock-suspend") == 0) {
            config.suspend_qualified = true;
        } else if (strcmp(argv[i], "--mock-resume") == 0) {
            resume_on_boot = true;
        } else if (strcmp(argv[i], "--held-at-boot") == 0) {
            held_at_boot = true;
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script_path = argv[++i];
        } else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            const char *end;
            if (!parse_u64(argv[++i], &end, &latency_ms) || *end != '\0' || latency_ms > 60000U) {
                fprintf(stderr, "Invalid delay (0..60000 ms)\n");
                return EXIT_FAILURE;
            }
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    app.scripted = script_path != NULL;
    if (app.scripted && !load_script(&app, script_path)) {
        return EXIT_FAILURE;
    }
    lock_fd = acquire_lock();
    if (lock_fd < 0) {
        perror("acquire mock daemon singleton lock");
        goto cleanup;
    }
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
        sigaddset(&signals, SIGTERM) != 0 ||
        sigprocmask(SIG_BLOCK, &signals, &previous_signals) != 0) {
        perror("block termination signals");
        goto cleanup;
    }
    signals_blocked = true;
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (epoll_fd < 0 || timer_fd < 0 || signal_fd < 0 || !add_fd(epoll_fd, timer_fd, FD_TIMER) ||
        !add_fd(epoll_fd, signal_fd, FD_SIGNAL)) {
        perror("initialize epoll sources");
        goto cleanup;
    }
    if (!app.scripted) {
        input_flags = fcntl(STDIN_FILENO, F_GETFL);
        if (input_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK) != 0 ||
            !add_fd(epoll_fd, STDIN_FILENO, FD_INPUT)) {
            fprintf(stderr, "stdin must be a terminal or pipe; use --script for regular files\n");
            goto cleanup;
        }
    }
    if (!monotonic_ms(&now_ms)) {
        perror("monotonic clock");
        goto cleanup;
    }
    app.started_at_ms = now_ms;
    app.running = true;
    cs_fake_init(&app.fake, latency_ms);
    app.fake.resume_on_boot = resume_on_boot;
    cs_power_init(&app.power, held_at_boot, now_ms);
    ports.context = &app;
    ports.submit = submit_request;
    ports.trace = trace_transition;
    if (!cs_fsm_init(&app.fsm, &config, &ports)) {
        fprintf(stderr, "Invalid FSM configuration\n");
        goto cleanup;
    }
    app.fsm.key_down = held_at_boot;
    printf("console-sessiond 0.2.0 MOCK ONLY; simulated suspend=%s\n",
           config.suspend_qualified ? "enabled" : "disabled");
    cs_fsm_start(&app.fsm, now_ms);
    while (app.running) {
        int count;
        if (!monotonic_ms(&now_ms)) {
            perror("monotonic clock");
            goto cleanup;
        }
        if (!pump_due(&app, now_ms)) {
            break;
        }
        if (!arm_timer(timer_fd, next_deadline(&app))) {
            perror("arm timerfd");
            goto cleanup;
        }
        count = epoll_wait(epoll_fd, ready, (int)(sizeof(ready) / sizeof(ready[0])), -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            goto cleanup;
        }
        if (!monotonic_ms(&now_ms)) {
            perror("monotonic clock");
            goto cleanup;
        }
        for (i = 0; i < count && app.running; ++i) {
            if (ready[i].data.u32 == (uint32_t)FD_TIMER) {
                if (!drain_timer(timer_fd)) {
                    perror("read timerfd");
                    goto cleanup;
                }
            } else if (ready[i].data.u32 == (uint32_t)FD_SIGNAL) {
                struct signalfd_siginfo info;
                const ssize_t bytes = read(signal_fd, &info, sizeof(info));
                if (bytes == (ssize_t)sizeof(info)) {
                    printf("STOP signal=%" PRIu32 " (mock process exit only)\n", info.ssi_signo);
                    app.running = false;
                } else if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
                    perror("read signalfd");
                    goto cleanup;
                }
            } else if (ready[i].data.u32 == (uint32_t)FD_INPUT) {
                if (!consume_input(&app, now_ms)) {
                    perror("mock input");
                    goto cleanup;
                }
            }
        }
    }
    exit_code = app.failed ? EXIT_FAILURE : EXIT_SUCCESS;
cleanup:
    if (input_flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, input_flags);
    }
    if (signal_fd >= 0) {
        (void)close(signal_fd);
    }
    if (timer_fd >= 0) {
        (void)close(timer_fd);
    }
    if (epoll_fd >= 0) {
        (void)close(epoll_fd);
    }
    if (lock_fd >= 0) {
        (void)close(lock_fd);
    }
    if (signals_blocked) {
        (void)sigprocmask(SIG_SETMASK, &previous_signals, NULL);
    }
    return exit_code;
}

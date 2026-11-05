/* Real Linux adapters. The separate console-sessiond-mock target owns all fakes. */
#include "sessiond/evdev.h"
#include "sessiond/profile.h"
#include "sessiond/worker.h"
#ifdef HAVE_CONSOLE_SUPERVISOR
#include "supervisor/managed.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

enum source { SOURCE_TIMER = 1, SOURCE_SIGNAL, SOURCE_INPUT, SOURCE_WORKER };
struct daemon_context {
    struct cs_profile profile;
    struct cs_fsm fsm;
    struct cs_power_input power;
    struct cs_evdev input;
    struct cs_worker worker;
    int epoll_fd;
    uint64_t input_retry_ms, media_retry_ms;
    bool attach_attempted;
};
static bool clock_ms(uint64_t *value) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) {
        return false;
    }
    *value = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
    return true;
}
static bool add_source(int epoll_fd, int fd, enum source source) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u32 = (uint32_t)source;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}
static void trace(void *context, enum cs_state from, enum cs_state to, const char *reason) {
    struct daemon_context *daemon = context;
    fprintf(stderr, "STATE %s -> %s session=%" PRIu64 " committed=%" PRIu64 " reason=%s\n",
            cs_state_name(from), cs_state_name(to), daemon->fsm.session_id,
            daemon->fsm.committed_generation, reason);
}
static bool submit(void *context, const struct cs_request *request, uint64_t now_ms) {
    struct daemon_context *daemon = context;
    struct cs_job job = {0};
    (void)now_ms;
    job.request = *request;
    job.active_game = daemon->fsm.active_game;
    return cs_worker_submit(&daemon->worker, &job);
}
static void input_event(void *context, const struct cs_event *event, uint64_t now_ms) {
    struct daemon_context *daemon = context;
    cs_fsm_dispatch(&daemon->fsm, event, now_ms);
}
static void emit(struct daemon_context *daemon, enum cs_event_kind kind, uint64_t now_ms) {
    struct cs_event event = {0};
    event.kind = kind;
    cs_fsm_dispatch(&daemon->fsm, &event, now_ms);
}
static bool reopen_input(struct daemon_context *daemon, uint64_t now_ms) {
    bool held;
    const char *path =
        strcmp(daemon->profile.input_path, "auto") == 0 ? NULL : daemon->profile.input_path;
    if (!cs_evdev_open(&daemon->input, path, daemon->profile.input_name, daemon->profile.key_code,
                       &held)) {
        return false;
    }
    if (!add_source(daemon->epoll_fd, daemon->input.fd, SOURCE_INPUT)) {
        cs_evdev_close(&daemon->input);
        return false;
    }
    daemon->input.opened_ms = now_ms;
    cs_power_init(&daemon->power, held, now_ms);
    daemon->power.long_ms = 3000U; /* Zero 28 port profile: requested three-second hold. */
    daemon->fsm.key_down = held;
    daemon->input_retry_ms = CS_TIME_NEVER;
    if (!daemon->fsm.started) {
        cs_fsm_start(&daemon->fsm, now_ms);
    }
    fprintf(stderr, "evdev ready: %s code=%u held=%d\n", daemon->input.name, daemon->input.code,
            held ? 1 : 0);
    return true;
}
static int singleton(const char *path) {
    struct stat st;
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        flock(fd, LOCK_EX | LOCK_NB) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
static bool instance_id(char hex[33]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char bytes[16];
    size_t used = 0U, i;
    while (used < sizeof(bytes)) {
        ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0U);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        used += (size_t)n;
    }
    for (i = 0U; i < sizeof(bytes); ++i) {
        hex[2U * i] = digits[bytes[i] >> 4U];
        hex[2U * i + 1U] = digits[bytes[i] & 15U];
    }
    hex[32] = '\0';
    return true;
}
static bool arm_timer(int fd, uint64_t deadline) {
    struct itimerspec timer;
    memset(&timer, 0, sizeof(timer));
    if (deadline != CS_TIME_NEVER) {
        timer.it_value.tv_sec = (time_t)(deadline / 1000U);
        timer.it_value.tv_nsec = (long)(deadline % 1000U) * 1000000L;
        if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0) {
            timer.it_value.tv_nsec = 1L;
        }
    }
    return timerfd_settime(fd, TFD_TIMER_ABSTIME, &timer, NULL) == 0;
}
int main(int argc, char **argv) {
#ifdef HAVE_CONSOLE_SUPERVISOR
    if (argc > 1 && strcmp(argv[1], "--supervised") == 0) {
        return console_sessiond_managed_main(argc, argv);
    }
#endif
    static struct daemon_context daemon;
    struct cs_ports ports;
    struct cs_config config = cs_config_default();
    struct epoll_event ready[4];
    int timer_fd = -1, signal_fd = -1, lock_fd = -1, exit_code = 1, i;
    bool worker_ready = false, running = true, mask_saved = false, shutdown_requested = false;
    sigset_t signals, previous;
    const char *profile_path = NULL, *lock_path = "/run/console-sessiond.lock";
    char instance[33];
    uint64_t now;
    daemon.epoll_fd = -1;
    cs_evdev_init(&daemon.input);
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_path = argv[++i];
        } else if (strcmp(argv[i], "--lock-file") == 0 && i + 1 < argc) {
            lock_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("console-sessiond --profile FILE [--lock-file FILE]\nReal evdev + checkpoint "
                 "socket + storage adapters. Board suspend/poweroff backend is disabled.");
            return 0;
        } else {
            fprintf(stderr, "Invalid argument\n");
            return 1;
        }
    }
    if (profile_path == NULL || !cs_profile_load(profile_path, &daemon.profile)) {
        fprintf(stderr, "Invalid/unreadable trusted profile\n");
        return 1;
    }
    lock_fd = singleton(lock_path);
    if (lock_fd < 0) {
        perror("singleton lock");
        goto cleanup;
    }
    if (!instance_id(instance)) {
        perror("getrandom instance ID");
        goto cleanup;
    }
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
        sigaddset(&signals, SIGTERM) != 0 || sigprocmask(SIG_BLOCK, &signals, &previous) != 0) {
        goto cleanup;
    }
    mask_saved = true;
    daemon.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (daemon.epoll_fd < 0 || timer_fd < 0 || signal_fd < 0 ||
        !add_source(daemon.epoll_fd, timer_fd, SOURCE_TIMER) ||
        !add_source(daemon.epoll_fd, signal_fd, SOURCE_SIGNAL)) {
        goto cleanup;
    }
    if (!cs_worker_init(&daemon.worker, &daemon.profile, instance)) {
        goto cleanup;
    }
    worker_ready = true;
    if (!add_source(daemon.epoll_fd, daemon.worker.event_fd, SOURCE_WORKER)) {
        goto cleanup;
    }
    ports.context = &daemon;
    ports.submit = submit;
    ports.trace = trace;
    if (!cs_fsm_init(&daemon.fsm, &config, &ports) || !clock_ms(&now)) {
        goto cleanup;
    }
    daemon.input_retry_ms = now;
    daemon.media_retry_ms = CS_TIME_NEVER;
    while (running) {
        uint64_t next;
        int count;
        struct cs_event event;
        if (!clock_ms(&now)) {
            goto cleanup;
        }
        if (daemon.input.fd < 0 && now >= daemon.input_retry_ms) {
            if (!reopen_input(&daemon, now)) {
                fprintf(stderr, "Power input unavailable/ambiguous; retry in 5 seconds\n");
                daemon.input_retry_ms = cs_deadline_after(now, 5000U);
            }
        }
        if (daemon.fsm.started) {
            emit(&daemon, CS_EV_TICK, now);
            /* Drain input before advancing hold/release timers: queued releases win. */
            if (daemon.input.fd >= 0) {
                bool more = true;
                unsigned int batches;
                for (batches = 0U; batches < 8U && more && daemon.input.fd >= 0; ++batches) {
                    bool reconciled = false, held = false;
                    if (!cs_evdev_drain(&daemon.input, &daemon.power, now, &reconciled, &held,
                                        &more, input_event, &daemon)) {
                        (void)epoll_ctl(daemon.epoll_fd, EPOLL_CTL_DEL, daemon.input.fd, NULL);
                        cs_evdev_close(&daemon.input);
                        daemon.input_retry_ms = cs_deadline_after(now, 5000U);
                        emit(&daemon, CS_EV_DEVICE_FAULT, now);
                    } else if (reconciled) {
                        if (held) {
                            daemon.fsm.key_down = true;
                        } else {
                            emit(&daemon, CS_EV_POWER_UP, now);
                        }
                    }
                }
                while (!more && daemon.input.fd >= 0 && cs_power_poll(&daemon.power, now, &event)) {
                    input_event(&daemon, &event, now);
                }
            }
            while (cs_worker_result(&daemon.worker, &event)) {
                if (event.reply_to.operation == CS_OP_BOOT &&
                    (event.flags & CS_REPLY_MEDIA_UNAVAILABLE) != 0U) {
                    daemon.media_retry_ms = cs_deadline_after(now, 5000U);
                }
                cs_fsm_dispatch(&daemon.fsm, &event, now);
                if (daemon.input.fd >= 0 && daemon.fsm.state == CS_MENU_READY &&
                    daemon.profile.attach_running && !daemon.attach_attempted) {
                    daemon.attach_attempted = true;
                    emit(&daemon, CS_EV_LAUNCH, now);
                }
            }
            if (shutdown_requested && !daemon.fsm.power_pending) {
                emit(&daemon, CS_EV_SYSTEM_SHUTDOWN, now);
            }
            if (daemon.fsm.state == CS_OFF) {
                break;
            }
            if (now >= daemon.media_retry_ms) {
                daemon.media_retry_ms = CS_TIME_NEVER;
                if (daemon.input.fd >= 0 && daemon.fsm.state == CS_RECOVERY &&
                    !daemon.fsm.active_game && !daemon.fsm.writer_unsettled) {
                    emit(&daemon, CS_EV_RETRY, now);
                }
            }
        }
        next = daemon.input_retry_ms;
        if (daemon.fsm.deadline_ms < next) {
            next = daemon.fsm.deadline_ms;
        }
        if (daemon.media_retry_ms < next) {
            next = daemon.media_retry_ms;
        }
        if (daemon.input.fd >= 0 && cs_power_deadline(&daemon.power) < next) {
            next = cs_power_deadline(&daemon.power);
        }
        if (!arm_timer(timer_fd, next)) {
            goto cleanup;
        }
        count = epoll_wait(daemon.epoll_fd, ready, 4, -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto cleanup;
        }
        for (i = 0; i < count; ++i) {
            if (ready[i].data.u32 == SOURCE_SIGNAL) {
                struct signalfd_siginfo info;
                ssize_t n = read(signal_fd, &info, sizeof(info));
                if (n == (ssize_t)sizeof(info)) {
                    /* A signal requests an orderly transaction; it is not proof
                     * that an in-flight emulator/storage writer has completed. */
                    shutdown_requested = true;
                    emit(&daemon, CS_EV_SYSTEM_SHUTDOWN, now);
                } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                    goto cleanup;
                }
            } else if (ready[i].data.u32 == SOURCE_TIMER || ready[i].data.u32 == SOURCE_WORKER) {
                uint64_t value;
                int fd = ready[i].data.u32 == SOURCE_TIMER ? timer_fd : daemon.worker.event_fd;
                ssize_t n;
                do {
                    n = read(fd, &value, sizeof(value));
                } while (n < 0 && errno == EINTR);
                if (n != (ssize_t)sizeof(value) && !(n < 0 && errno == EAGAIN)) {
                    goto cleanup;
                }
            }
            /* Input is drained at the top, before evaluating timers. */
        }
    }
    exit_code = 0;
cleanup:
    if (exit_code != 0) {
        fprintf(stderr, "Daemon setup/runtime error: errno=%d\n", errno);
    }
    cs_evdev_close(&daemon.input);
    if (worker_ready) {
        cs_worker_stop(&daemon.worker);
    }
    if (signal_fd >= 0) {
        (void)close(signal_fd);
    }
    if (timer_fd >= 0) {
        (void)close(timer_fd);
    }
    if (daemon.epoll_fd >= 0) {
        (void)close(daemon.epoll_fd);
    }
    if (lock_fd >= 0) {
        (void)close(lock_fd);
    }
    if (mask_saved) {
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    }
    return exit_code;
}

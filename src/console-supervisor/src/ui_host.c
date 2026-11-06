#include "launcher/app.h"
#include "supervisor/wire.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

struct ui_host
{
    int signal_fd;
    struct sc_outbox out;
    struct sc_message received;
    uint64_t base, sequence;
    bool disconnected;
};
static bool request(struct ui_host *h, struct sc_message *message)
{
    if (h->disconnected || h->sequence == UINT32_MAX)
    {
        return false;
    }
    message->token = h->base + (++h->sequence);
    return sc_queue(&h->out, message);
}
static bool poll_update(void *context, struct cl_update *update)
{
    struct ui_host *h = context;
    struct signalfd_siginfo signal;
    int received;
    if (read(h->signal_fd, &signal, sizeof(signal)) == (ssize_t)sizeof(signal) || h->disconnected)
    {
        update->kind = CL_UPDATE_QUIESCE;
        return true;
    }
    if (!sc_flush(3, &h->out))
    {
        h->disconnected = true;
        update->kind = CL_UPDATE_QUIESCE;
        return true;
    }
    received = sc_receive(3, &h->received);
    if (received == 0)
    {
        return false;
    }
    if (received < 0)
    {
        h->disconnected = true;
        update->kind = CL_UPDATE_QUIESCE;
        return true;
    }
    switch (h->received.type)
    {
    case SC_UI_HISTORY:
        update->kind = CL_UPDATE_HISTORY;
        update->history = &h->received.data.history;
        break;
    case SC_UI_LIBRARY:
        update->kind = CL_UPDATE_LIBRARY;
        update->history = &h->received.data.history;
        break;
    case SC_UI_TOOLS:
        update->kind = CL_UPDATE_TOOLS;
        update->tools = &h->received.data.tools;
        break;
    case SC_UI_STATES:
        update->kind = CL_UPDATE_STATES;
        update->states = &h->received.data.states;
        break;
    case SC_UI_STATUS:
        update->kind = CL_UPDATE_STATUS;
        update->battery_percent = h->received.data.telemetry.battery;
        update->external_power = h->received.data.telemetry.usb;
        update->brightness_level = h->received.data.telemetry.brightness;
        update->volume_level = h->received.data.telemetry.volume;
        update->transaction_busy = h->received.data.telemetry.busy;
        break;
    case SC_UI_ERROR:
        update->kind = CL_UPDATE_ERROR;
        memcpy(update->message, h->received.data.text, sizeof(update->message));
        update->message[sizeof(update->message) - 1U] = '\0';
        break;
    case SC_UI_INPUT_READY:
        update->kind = CL_UPDATE_INPUT_READY;
        update->input_persisted = h->received.data.result == CS_RESULT_COMPLETED;
        break;
    case SC_UI_QUIESCE:
        update->kind = CL_UPDATE_QUIESCE;
        break;
    default:
        h->disconnected = true;
        update->kind = CL_UPDATE_QUIESCE;
        break;
    }
    return true;
}
static bool states(void *context, const struct cl_entry *entry, uint64_t revision)
{
    struct sc_message m;
    sc_message_init(&m, SC_UI_PROBE, 0U);
    m.data.probe.entry = *entry;
    m.data.probe.revision = revision;
    return request(context, &m);
}
static bool remove_history(void *context, const char game[65])
{
    struct sc_message m;
    sc_message_init(&m, SC_REMOVE, 0U);
    memcpy(m.data.text, game, 65U);
    return request(context, &m);
}
static bool input_verification(void *context, const struct cl_input_verification *verification)
{
    struct sc_message message;
    sc_message_init(&message, SC_UI_INPUT_VERIFIED, 0U);
    message.data.verification = *verification;
    return request(context, &message);
}
static bool delete_states(void *context, const char game[65])
{
    struct sc_message m;
    sc_message_init(&m,SC_DELETE_STATES,0U);
    memcpy(m.data.text,game,65U);
    return request(context,&m);
}
static bool tool_request(void *context, const struct cl_tool_request *tool)
{
    struct sc_message message;
    sc_message_init(&message, SC_UI_TOOL, 0U);
    message.data.tool_request = *tool;
    return request(context, &message);
}
static bool library(void *context)
{
    struct sc_message m;
    sc_message_init(&m, SC_UI_LIBRARY, 0U);
    return request(context, &m);
}
static bool library_page(void *context, const struct cl_library_query *query)
{
    struct sc_message message;
    if (query == NULL || query->favorite_count > CL_HISTORY_MAX)
        return false;
    sc_message_init(&message, SC_UI_LIBRARY, 0U);
    message.data.history.library_mode = query->mode;
    message.data.history.library_expert = query->expert;
    message.data.history.library_search_tile = query->search_tile;
    message.data.history.library_search_results = query->search_results;
    message.data.history.library_offset = query->offset;
    message.data.history.library_query_id = query->id;
    message.data.history.library_page_size = query->page_size;
    /* Existing runtime-only request envelope: MAX requests an explicit rescan;
     * zero permits a snapshot hit. The response carries the actual revision. */
    message.data.history.revision = query->refresh ? UINT64_MAX : 0U;
    strcpy(message.data.history.library_system, query->system);
    strcpy(message.data.history.library_keyword, query->keyword);
    strcpy(message.data.history.library_anchor, query->anchor);
    message.data.history.count = query->favorite_count;
    for (size_t i = 0U; i < query->favorite_count; ++i)
        strcpy(message.data.history.entries[i].identity.game, query->favorites[i]);
    return request(context, &message);
}
static bool level(void *context, bool brightness, int direction)
{
    struct sc_message m;
    sc_message_init(&m, SC_UI_LEVEL, 0U);
    m.data.level.brightness = brightness;
    m.data.level.direction = direction;
    return request(context, &m);
}
static bool display_ready(void *context)
{
    bool queued = level(context, false, 0);
    uint64_t uptime;
    /* The launcher invokes this only after its first successful present.
     * Measure usable UI, rather than treating process creation as boot done. */
    if (queued && sc_now(&uptime))
        fprintf(stderr, "ui timing: stage=first-frame uptime_ms=%llu\n",
                (unsigned long long)uptime);
    return queued;
}
static bool system_request(void *context, bool shutdown)
{
    struct sc_message m;
    sc_message_init(&m, SC_EVENT, 0U);
    m.data.event = shutdown ? CS_EV_SYSTEM_SHUTDOWN : CS_EV_RETRY;
    return request(context, &m);
}
int main(void)
{
    static struct ui_host host;
    static struct sc_message initial, result;
    static struct cl_history history;
    struct cl_app_config config;
    struct cl_services services = {0};
    struct cl_ui_memory memory;
    struct cl_launch_intent intent = {0};
    sigset_t signals;
    uint64_t now, end;
    enum cl_run_result run;
    host.signal_fd = -1;
    if (!sc_peer(3, geteuid()) || !sc_now(&now))
    {
        return 1;
    }
    end = now + 30000U;
    for (;;)
    {
        struct pollfd fd = {3, POLLIN, 0};
        int n = sc_receive(3, &initial);
        if (n == 1)
        {
            break;
        }
        if (n < 0 || !sc_now(&now) || now >= end)
        {
            return 1;
        }
        if (poll(&fd, 1U, 100) < 0 && errno != EINTR)
        {
            return 1;
        }
    }
    if (initial.type != SC_UI_INIT || (initial.token & UINT32_MAX) != 0U)
    {
        return 1;
    }
    host.base = initial.token;
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTERM) != 0 ||
        sigaddset(&signals, SIGINT) != 0 || sigprocmask(SIG_BLOCK, &signals, NULL) != 0)
    {
        return 1;
    }
    host.signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (host.signal_fd < 0)
    {
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.display.display_index = initial.data.ui.display;
    config.display.rotation = (enum cl_rotation)initial.data.ui.rotation;
    config.display.portrait_rotation = (enum cl_rotation)initial.data.ui.portrait_rotation;
    config.display.qualified_software_fallback = initial.data.ui.software_fallback;
    config.input = initial.data.ui.input;
    config.input_verified = initial.data.ui.input_verified;
    config.start_gameswitcher = initial.data.ui.start_gameswitcher;
#ifdef CL_START_GAMESWITCHER
    config.start_gameswitcher = true;
#endif
    memcpy(config.icon_theme, initial.data.ui.icon_theme, sizeof(config.icon_theme));
    config.icon_theme[sizeof(config.icon_theme) - 1U] = '\0';
    memcpy(config.icon_pack, initial.data.ui.icon_pack, sizeof(config.icon_pack));
    config.icon_pack[sizeof(config.icon_pack) - 1U] = '\0';
    memcpy(config.theme_path, initial.data.ui.theme_path, sizeof(config.theme_path));
    memcpy(config.theme_overrides, initial.data.ui.theme_overrides, sizeof(config.theme_overrides));
    config.theme_path[sizeof(config.theme_path) - 1U] = '\0';
    config.theme_overrides[sizeof(config.theme_overrides) - 1U] = '\0';
    memcpy(config.card_path, initial.data.ui.card_path, sizeof(config.card_path));
    memcpy(config.card_uuid, initial.data.ui.card_uuid, sizeof(config.card_uuid));
    config.card_path[sizeof(config.card_path) - 1U] = '\0';
    config.card_uuid[sizeof(config.card_uuid) - 1U] = '\0';
    config.qualified_fat_syncfs = initial.data.ui.qualified_fat_syncfs;
    memory = initial.data.ui.memory;
    services.context = &host;
    services.poll = poll_update;
    services.request_states = states;
    services.request_remove_history = remove_history;
    services.request_delete_states = delete_states;
    services.request_library = library;
    services.request_library_page = library_page;
    services.request_tool = tool_request;
    services.request_input_verification = input_verification;
    services.adjust_level = level;
    services.display_ready = display_ready;
    services.request_system = system_request;
    initial.data.ui.error[sizeof(initial.data.ui.error) - 1U] = '\0';
    run = cl_launcher_run(&config, &services, &history, &memory, initial.data.ui.error, &intent);
    fprintf(stderr, "launcher host: run_complete=%d input_persisted=%u epoch=%llu\n",
            (int)run, memory.input_persisted ? 1U : 0U,
            (unsigned long long)(host.base >> 32));
    sc_message_init(&result, SC_UI_RESULT, host.base);
    result.data.ui_result.result = (int)run;
    result.data.ui_result.memory = memory;
    result.data.ui_result.intent = intent;
    bool handoff = sc_ui_handoff(3, &host.out, &result, 5000U);
    if (run == CL_RUN_FAILURE)
    {
        fprintf(stderr, "launcher host: display/input/IPC initialization failed\n");
    }
    if (!handoff)
        fprintf(stderr, "launcher host: handoff not acknowledged pending=%zu errno=%d\n",
                host.out.count, errno);
    (void)close(host.signal_fd);
    (void)close(3);
    sc_discard(&host.out);
    return run == CL_RUN_FAILURE || !handoff ? 1 : 0;
}

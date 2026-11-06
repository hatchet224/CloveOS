#include "launcher/i18n.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "supervisor/lan_services.h"
#include "supervisor/process.h"
#include "supervisor/wire.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

static const struct {
    const char *name, *id, *mountpoint;
    unsigned int port;
} protocols[] = {
    {"SMB", "smb", "/run/console/lan-smb", 445},
    {"SFTP", "sftp", "/run/console/lan-sftp", 2222}
};

void sc_lan_init(struct sc_lan_services *s)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) s->endpoints[i].control = -1;
}

static void disconnect(struct sc_lan_endpoint *e)
{
    if (e->control >= 0) (void)close(e->control);
    e->control = -1;
    e->ready = false;
    e->fingerprint[0] = '\0';
}

static void stop_endpoint(struct sc_lan_endpoint *e, uint64_t now)
{
    if (e->pid <= 0 || e->stopping) return;
    disconnect(e);
    e->stopping = true;
    e->deadline = now + 12000U;
}

static bool password(char out[13])
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned char bytes[12];
    if (getrandom(bytes, sizeof(bytes), GRND_NONBLOCK) != (ssize_t)sizeof(bytes)) return false;
    for (size_t i = 0; i < sizeof(bytes); ++i) out[i] = alphabet[bytes[i] & 31U];
    out[12] = '\0';
    return true;
}

static bool wifi_address(char address[INET_ADDRSTRLEN])
{
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) return false;
    for (struct ifaddrs *it = interfaces; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET ||
            strcmp(it->ifa_name, "wlan0") != 0 || (it->ifa_flags & IFF_UP) == 0U) continue;
        const struct sockaddr_in *ip = (const struct sockaddr_in *)it->ifa_addr;
        if (ip->sin_addr.s_addr == INADDR_ANY) continue;
        if (inet_ntop(AF_INET, &ip->sin_addr, address, INET_ADDRSTRLEN) != NULL) break;
    }
    freeifaddrs(interfaces);
    return true;
}

static bool start_endpoint(struct sc_lan_endpoint *e, size_t i, const char *card,
                           const char *address, uint64_t now)
{
    char listen[INET_ADDRSTRLEN + 8U];
    (void)snprintf(listen, sizeof(listen), "%s:%u", address, protocols[i].port);
    char *args[] = {"/system/bin/console-lan-server", (char *)protocols[i].id, listen, (char *)card, NULL};
    int pair[2];
    if (!sc_pair(pair)) return false;
    bool started = send(pair[0], e->password, 12U, MSG_NOSIGNAL) == 12 &&
        sc_spawn(args[0], args, pair[1], -1, NULL, &e->pid);
    (void)close(pair[1]);
    if (!started) {
        (void)close(pair[0]);
        return false;
    }
    e->control = pair[0];
    e->deadline = now + 6000U;
    return true;
}

bool sc_lan_busy(const struct sc_lan_services *s)
{
    return s->endpoints[0].pid > 0 || s->endpoints[1].pid > 0;
}

bool sc_lan_reap(struct sc_lan_services *s, pid_t pid, bool success)
{
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        struct sc_lan_endpoint *e = &s->endpoints[i];
        if (e->pid != pid) continue;
        if ((!e->stopping || !success) && e->wanted) e->failed = true;
        disconnect(e);
        e->pid = 0;
        e->stopping = false;
        /* The child normally unmounts this itself. Release any mounts left
         * behind by a crash before storage can be unmounted or exported. */
        (void)umount2(protocols[i].mountpoint, MNT_DETACH);
        (void)rmdir(protocols[i].mountpoint);
        return true;
    }
    return false;
}

void sc_lan_update(struct sc_lan_services *s, const char *card, uint64_t now, bool paused)
{
    if (paused) s->next_scan = 0U;
    if (!paused && now >= s->next_scan && (s->endpoints[0].wanted || s->endpoints[1].wanted)) {
        char address[INET_ADDRSTRLEN] = "";
        s->next_scan = now + 1000U;
        if (wifi_address(address) && strcmp(address, s->address) != 0) {
            strcpy(s->address, address);
            for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
                stop_endpoint(&s->endpoints[i], now);
                s->endpoints[i].failed = false;
            }
        }
    }
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        struct sc_lan_endpoint *e = &s->endpoints[i];
        if (paused || !e->wanted || !s->address[0]) stop_endpoint(e, now);
        if (e->stopping) {
            if (now >= e->deadline) {
                e->failed = e->wanted;
                (void)kill(e->pid, SIGKILL);
                e->deadline = UINT64_MAX;
            }
            continue;
        }
        if (e->pid > 0 && !e->ready) {
            char packet[64];
            ssize_t size = recv(e->control, packet, sizeof(packet) - 1U, MSG_DONTWAIT);
            if (size > 0 && packet[0] == 'R' && (i == 0 ? size == 1 : size == 51)) {
                packet[size] = '\0';
                strcpy(e->fingerprint, packet + 1);
                e->ready = true;
            } else if (now >= e->deadline) {
                e->failed = true;
                stop_endpoint(e, now);
            }
        } else if (e->pid <= 0 && !paused && e->wanted && !e->failed && s->address[0]) {
            e->failed = !start_endpoint(e, i, card, s->address, now);
        }
    }
}

void sc_lan_request(struct sc_lan_services *s, const char *card, uint64_t now, bool paused,
                    enum cl_tool_operation operation, struct cl_tool_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = CL_TOOL_LAN_SERVICES;
    out->ready = true;
    if (operation != CL_TOOL_LOAD_LAN_SERVICES) {
        struct sc_lan_endpoint *e = &s->endpoints[operation == CL_TOOL_TOGGLE_LAN_SFTP ? 1 : 0];
        if (e->wanted) {
            e->wanted = false;
            e->password[0] = '\0';
        } else {
            e->wanted = password(e->password);
            e->failed = !e->wanted;
        }
        s->next_scan = 0U;
    }
    sc_lan_update(s, card, now, paused);
    strcpy(out->status, "Wi-Fi file sharing for Roms and BIOS. Each service is off after reboot.");
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        const struct sc_lan_endpoint *e = &s->endpoints[i];
        struct cl_tool_row *row = &out->rows[out->count++];
        strcpy(row->id, protocols[i].id);
        row->available = true;
        const char *state = e->ready ? "On" : e->stopping ? "Stopping" : e->failed ? "Error" :
            !e->wanted ? "Off" : paused ? "Paused" : !s->address[0] ? "Waiting for Wi-Fi" : "Starting";
        (void)snprintf(row->label, sizeof(row->label), "%s: %s", protocols[i].name, state);
        if (e->ready) {
            if (i == 0) (void)snprintf(row->label, sizeof(row->label), "SMB  \\\\%s\\CloveOS", s->address);
            else (void)snprintf(row->label, sizeof(row->label), "SFTP  %s:%u", s->address, protocols[i].port);
            (void)snprintf(row->detail, sizeof(row->detail), CL_TR("User: clove  Password: %s"), e->password);
        } else if (e->failed) {
            strcpy(row->detail, "Startup failed. Toggle off, then on to retry. Check Wi-Fi and the SD card.");
        } else {
            strcpy(row->detail, i == 0 ? "Connect through Windows File Explorer or an SMB client." :
                "Connect with an SFTP client using password authentication.");
        }
    }
    if (s->endpoints[1].ready) {
        struct cl_tool_row *row = &out->rows[out->count++];
        strcpy(row->id, "fingerprint");
        strcpy(row->label, "SFTP host key fingerprint");
        strcpy(row->detail, s->endpoints[1].fingerprint);
    }
}

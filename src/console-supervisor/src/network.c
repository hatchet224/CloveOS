#include "sessiond/durability.h"
#include "supervisor/network.h"
#include "supervisor/build.h"
#include "sessiond/document.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#define WPA_ENDPOINT "/run/console/wpa/wlan0"
#define WPA_CONFIG "/run/console/wpa.conf"
#define WPA_REPLY 32768U
struct connection { int fd; };
static bool connect_control(struct connection *c, const char *endpoint)
{
    struct sockaddr_un local = {0}, remote = {0};
    static unsigned int serial;
    c->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0 || strlen(endpoint) >= sizeof(remote.sun_path))
        return false;
    local.sun_family = remote.sun_family = AF_UNIX;
    /* Linux abstract local address leaves no stale pathname on a crash. */
    (void)snprintf(local.sun_path + 1, sizeof(local.sun_path) - 1U,
                   "console-wifi-%ld-%u", (long)getpid(), ++serial);
    strcpy(remote.sun_path, endpoint);
    if (bind(c->fd, (const struct sockaddr *)&local, sizeof(local)) == 0 &&
        connect(c->fd, (const struct sockaddr *)&remote, sizeof(remote)) == 0)
        return true;
    close(c->fd); c->fd = -1;
    return false;
}
static bool command(struct connection *c, const char *text, char *reply, size_t cap)
{
    struct pollfd p = {c->fd, POLLIN, 0};
    ssize_t n;
    if (send(c->fd, text, strlen(text), MSG_NOSIGNAL) != (ssize_t)strlen(text))
        return false;
    /* Each command has one bounded reply. A timeout discards this connection;
     * it must never consume a late reply as the next command's response. */
    if (poll(&p, 1U, 2000) != 1 || (p.revents & POLLIN) == 0)
        return false;
    n = recv(c->fd, reply, cap - 1U, MSG_TRUNC);
    if (n < 0 || (size_t)n >= cap - 1U)
        return false;
    reply[n] = '\0';
    return true;
}
static bool ok(struct connection *c, const char *text)
{
    char reply[64];
    return command(c, text, reply, sizeof(reply)) && strcmp(reply, "OK\n") == 0;
}
static void status(struct cl_tool_snapshot *s, const char *text)
{
    (void)snprintf(s->status, sizeof(s->status), "%s", text);
}
static void row(struct cl_tool_snapshot *s, const char *id, const char *label,
                  const char *detail, bool available)
{
    struct cl_tool_row *r;
    if (s->count >= CL_TOOL_ROWS) return;
    r = &s->rows[s->count++];
    (void)snprintf(r->id, sizeof(r->id), "%s", id);
    (void)snprintf(r->label, sizeof(r->label), "%s", label);
    (void)snprintf(r->detail, sizeof(r->detail), "%s", detail);
    r->available = available;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool mac_valid(const char *s)
{
    if (strlen(s) != 17U) return false;
    for (size_t i = 0U; i < 17U; ++i)
        if (i % 3U == 2U ? s[i] != ':' : hex_digit(s[i]) < 0) return false;
    return true;
}
static bool ssid_decode(const char *text, unsigned char bytes[32], size_t *length)
{
    size_t used = 0U;
    while (*text != '\0') {
        unsigned char c = (unsigned char)*text++;
        if (c == '\\') {
            char escaped = *text++;
            if (escaped == '\0') return false;
            if (escaped == 'x') {
                int a, b;
                if (text[0] == '\0' || text[1] == '\0' ||
                    (a = hex_digit(text[0])) < 0 || (b = hex_digit(text[1])) < 0) return false;
                c = (unsigned char)(a * 16 + b); text += 2;
            } else if (escaped == 'n') c = '\n';
            else if (escaped == 'r') c = '\r';
            else if (escaped == 't') c = '\t';
            else if (escaped == 'e') c = 27U;
            else if (escaped == '\\' || escaped == '"') c = (unsigned char)escaped;
            else return false;
        }
        if (used >= 32U) return false;
        bytes[used++] = c;
    }
    *length = used; return used != 0U;
}
static void ssid_label(const unsigned char *bytes, size_t n, char out[96])
{
    memcpy(out, bytes, n); out[n] = '\0';
    if (memchr(bytes, '\0', n) != NULL || !cs_utf8_valid(out, n)) {
        strcpy(out, "Network with non-text SSID"); return;
    }
    for (size_t i = 0U; i < n; ++i)
        if ((unsigned char)out[i] < 32U || out[i] == 127) out[i] = '?';
}
static bool security(const char *flags, bool *secured)
{
    *secured = strstr(flags, "WPA") != NULL || strstr(flags, "WEP") != NULL ||
               strstr(flags, "OWE") != NULL || strstr(flags, "SAE") != NULL ||
               strstr(flags, "RSN") != NULL;
    if (!*secured) return strstr(flags, "ESS") != NULL;
    /* This legacy WEXT radio build deliberately offers only WPA2-PSK/CCMP.
     * Enterprise, WEP and WPA3-only APs remain visible but cannot downgrade. */
    return (strstr(flags, "WPA2-PSK-CCMP") != NULL ||
            strstr(flags, "WPA2-PSK+SAE-CCMP") != NULL) && strstr(flags, "EAP") == NULL;
}
static char *field(char *text, const char *key)
{
    size_t n = strlen(key);
    char *p = text;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, n) == 0 && p[n] == '=') {
            char *end = strchr(p + n + 1U, '\n');
            if (end != NULL) *end = '\0';
            return p + n + 1U;
        }
        p = strchr(p, '\n'); if (p != NULL) ++p;
    }
    return NULL;
}
static void scan_rows(struct connection *c, struct cl_tool_snapshot *s)
{
    char *reply = malloc(WPA_REPLY), *save = NULL, *line;
    if (reply == NULL) { status(s, "Not enough memory to list networks."); return; }
    if (!command(c, "SCAN_RESULTS", reply, WPA_REPLY)) {
        status(s, "Wi-Fi scan reply timed out. Use Refresh to retry."); free(reply); return;
    }
    line = strtok_r(reply, "\n", &save); /* Column header. */
    while (line != NULL && (line = strtok_r(NULL, "\n", &save)) != NULL &&
           s->count < CL_TOOL_ROWS) {
        char *columns[5], label[96], detail[128], *p = line;
        unsigned char ssid[32]; size_t n; bool secured;
        columns[0] = p;
        for (size_t i = 1U; i < 5U; ++i) {
            p = strchr(p, '\t'); if (p == NULL) break;
            *p++ = '\0'; columns[i] = p;
            if (i == 4U && mac_valid(columns[0]) && ssid_decode(columns[4], ssid, &n)) {
                bool supported = security(columns[3], &secured);
                ssid_label(ssid, n, label);
                (void)snprintf(detail, sizeof(detail), "%s  %.8s dBm  %.17s",
                    supported ? (secured ? "WPA2 password" : "Open network") : "Unsupported security",
                    columns[2], columns[0]);
                row(s, columns[0], label, detail, supported);
                s->rows[s->count - 1U].required = secured;
            }
        }
    }
    free(reply);
}
static bool persist_config(const struct cs_store *store)
{
    char bytes[65536], temp[96];
    struct stat st, before, live;
    size_t used = 0U, written = 0U;
    int input = -1, output = -1; bool success = false;
    static unsigned int serial;
    temp[0] = '\0';
    if (cs_store_check_media(store) != CS_STORE_OK || fstat(store->data_fd, &before) != 0)
        return false;
    input = open(WPA_CONFIG, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (input < 0 || fstat(input, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U ||
        st.st_size < 1 || st.st_size >= (off_t)sizeof(bytes)) goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(input, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    for (unsigned int i = 0U; i < 16U; ++i) {
        (void)snprintf(temp, sizeof(temp), ".wifi-%ld-%u.tmp", (long)getpid(), ++serial);
        output = openat(store->data_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output >= 0 || errno != EEXIST) break;
    }
    if (output < 0) goto done;
    while (written < used) {
        ssize_t n = write(output, bytes + written, used - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        written += (size_t)n;
    }
    if (fsync(output) != 0 || cs_store_check_media(store) != CS_STORE_OK ||
        fstatat(store->card_fd, ".console", &live, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(live.st_mode) || live.st_dev != before.st_dev || live.st_ino != before.st_ino)
        goto done;
    if (close(output) != 0) { output = -1; goto done; }
    output = -1;
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0 ||
        renameat(store->data_fd, temp, store->data_fd, "wifi.conf") != 0) goto done;
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0) goto done;
    success = cs_store_check_media(store) == CS_STORE_OK;
done:
    memset(bytes, 0, sizeof(bytes));
    if (input >= 0 && close(input) != 0) success = false;
    if (output >= 0) (void)close(output);
    if (temp[0] != '\0') (void)unlinkat(store->data_fd, temp, 0);
    return success;
}
static bool connect_network(struct connection *c, const struct cl_tool_request *r)
{
    char mac[18], query[256], reply[4096], flags_copy[4096], id[16], ssid_hex[65], psk[130];
    char *value; unsigned char ssid[32]; size_t n; bool secured;
    int network;
    if ((r->enabled_mask >> 48U) != 0U) return false;
    (void)snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned int)((r->enabled_mask >> 40U) & 255U), (unsigned int)((r->enabled_mask >> 32U) & 255U),
        (unsigned int)((r->enabled_mask >> 24U) & 255U), (unsigned int)((r->enabled_mask >> 16U) & 255U),
        (unsigned int)((r->enabled_mask >> 8U) & 255U), (unsigned int)(r->enabled_mask & 255U));
    (void)snprintf(query, sizeof(query), "BSS %s", mac);
    if (!command(c, query, reply, sizeof(reply))) return false;
    strcpy(flags_copy, reply);
    value = field(flags_copy, "flags");
    if (value == NULL || !security(value, &secured)) return false;
    value = field(reply, "ssid");
    if (value == NULL || !ssid_decode(value, ssid, &n)) return false;
    for (size_t i = 0U; i < n; ++i) (void)snprintf(ssid_hex + i * 2U, 3U, "%02x", ssid[i]);
    if (secured) {
        size_t length = strnlen(r->id, sizeof(r->id)), used = 0U;
        if (length < 8U || length > 63U) return false;
        psk[used++] = '"';
        for (size_t i = 0U; i < length; ++i) {
            unsigned char ch = (unsigned char)r->id[i];
            if (ch < 32U || ch > 126U) return false;
            if (ch == '"' || ch == '\\') psk[used++] = '\\';
            psk[used++] = (char)ch;
        }
        psk[used++] = '"'; psk[used] = '\0';
    }
    if (!command(c, "ADD_NETWORK", id, sizeof(id))) return false;
    { char *end; long v = strtol(id, &end, 10);
      if (end == id || v < 0 || v > 100000 || (*end != '\n' && *end != '\0')) return false;
      network = (int)v; }
    (void)snprintf(query, sizeof(query), "SET_NETWORK %d ssid %s", network, ssid_hex);
    if (!ok(c, query)) goto fail;
    (void)snprintf(query, sizeof(query), "SET_NETWORK %d key_mgmt %s", network, secured ? "WPA-PSK" : "NONE");
    if (!ok(c, query)) goto fail;
    if (secured) {
        (void)snprintf(query, sizeof(query), "SET_NETWORK %d proto RSN", network);
        if (!ok(c, query)) goto fail;
        (void)snprintf(query, sizeof(query), "SET_NETWORK %d pairwise CCMP", network);
        if (!ok(c, query)) goto fail;
        (void)snprintf(query, sizeof(query), "SET_NETWORK %d psk %s", network, psk);
        if (!ok(c, query)) goto fail;
        memset(query, 0, sizeof(query)); memset(psk, 0, sizeof(psk));
    }
    (void)snprintf(query, sizeof(query), "SELECT_NETWORK %d", network);
    if (ok(c, query)) return true;
fail:
    memset(psk, 0, sizeof(psk));
    /* Use a fresh request only after an actual reply; a timed-out command's
     * late response cannot establish success. Deinit closes the connection. */
    return false;
}
static void network_at(const struct cs_store *store, const struct cl_tool_request *r,
                        struct cl_tool_snapshot *s, const char *endpoint)
{
    struct connection c = {-1}; char reply[4096], state_copy[4096], *state, *ip;
    bool mutation = false, result = true;
    memset(s, 0, sizeof(*s)); s->kind = CL_TOOL_NETWORK; s->ready = true;
    if (!connect_control(&c, endpoint)) {
        if (c.fd >= 0) close(c.fd);
        status(s, access("/sys/class/net/wlan0", F_OK) == 0 ?
            "Radio detected; Wi-Fi service is unavailable. Check startup log." :
            "No wlan0 radio registered. The Wi-Fi driver is unavailable.");
        row(s, "@refresh", "Refresh Wi-Fi status", "A refreshes hardware/service detection", true);
        return;
    }
    if (r->operation == CL_TOOL_SCAN_NETWORK) result = ok(&c, "SCAN");
    else if (r->operation == CL_TOOL_CONNECT_NETWORK) { result = connect_network(&c, r); mutation = result; }
    else if (r->operation == CL_TOOL_DISCONNECT_NETWORK) result = ok(&c, "DISCONNECT");
    else if (r->operation == CL_TOOL_RECONNECT_NETWORK) result = ok(&c, "RECONNECT");
    else if (r->operation == CL_TOOL_FORGET_NETWORK) {
        result = ok(&c, "REMOVE_NETWORK all"); mutation = result;
    }
    if (!result) {
        status(s, "Wi-Fi request rejected or timed out. Refresh before retrying."); goto done;
    }
    if (mutation && (!ok(&c, "SAVE_CONFIG") || !persist_config(store))) {
        status(s, "Wi-Fi change is not durably saved. Check card before restarting."); goto done;
    }
    if (!command(&c, "STATUS", reply, sizeof(reply))) {
        status(s, "Wi-Fi service timed out. Refresh to retry."); goto done;
    }
    strcpy(state_copy, reply); state = field(state_copy, "wpa_state"); ip = field(reply, "ip_address");
    if (state != NULL && strcmp(state, "COMPLETED") == 0)
        (void)snprintf(s->status, sizeof(s->status), "%s | %s",
                       ip != NULL && strcmp(ip, "0.0.0.0") != 0 ? "Wi-Fi connected" : "Wi-Fi joined; waiting for DHCP",
                       ip != NULL && strcmp(ip, "0.0.0.0") != 0 ? ip : "Status refreshes automatically");
    else
        (void)snprintf(s->status, sizeof(s->status), "Wi-Fi: %s | X refreshes status", state != NULL ? state : "unknown");
    row(s, "@scan", "Scan for networks", "A scans; X refreshes results/status", true);
    row(s, "@disconnect", "Disconnect", "Stops this connection; saved networks are retained", true);
    row(s, "@reconnect", "Reconnect", "Reconnect using a saved network", true);
    row(s, "@forget", "Forget saved networks", "A opens confirmation; credentials are removed on Apply", true);
    scan_rows(&c, s);
done:
    if (s->count == 0U) row(s, "@refresh", "Refresh Wi-Fi status", "X or A retries status", true);
    close(c.fd);
}
void sc_network_request(const struct cs_store *store, const struct cl_tool_request *r,
                         struct cl_tool_snapshot *s)
{
    network_at(store, r, s, WPA_ENDPOINT);
}
bool sc_network_reset(const struct cs_store *store)
{
    struct connection c = {-1};
    struct stat st;
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    if (lstat(WPA_ENDPOINT, &st) == 0) {
        bool cleared = connect_control(&c, WPA_ENDPOINT) &&
                       ok(&c, "REMOVE_NETWORK all") && ok(&c, "SAVE_CONFIG");
        if (c.fd >= 0) close(c.fd);
        if (!cleared) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    return unlinkat(store->data_fd, "wifi.conf", 0) == 0 || errno == ENOENT;
}
void sc_device_snapshot(const struct cs_store *store, struct cl_tool_snapshot *s)
{
    struct utsname u; struct statvfs v; char detail[128];
    memset(s, 0, sizeof(*s)); s->kind = CL_TOOL_DEVICE; s->ready = true;
    row(s, "device", "MagicX Mini Zero 28", "Allwinner A133P / 640 x 480 logical display", true);
    if (uname(&u) == 0) {
        (void)snprintf(detail, sizeof(detail), "%.60s / %.60s", u.release, u.machine);
        row(s, "kernel", "Linux kernel", detail, true);
    }
    row(s, "card", "Game card", store->uuid, true);
    if (fstatvfs(store->card_fd, &v) == 0) {
        (void)snprintf(detail, sizeof(detail), "%" PRIu64 " MiB free / %" PRIu64 " MiB total",
            (uint64_t)v.f_bavail * v.f_frsize / 1048576U, (uint64_t)v.f_blocks * v.f_frsize / 1048576U);
        row(s, "space", "Storage", detail, true);
    }
    row(s, "radio", "Wi-Fi radio", access("/sys/class/net/wlan0", F_OK) == 0 ?
        "wlan0 detected" : "No wlan0 registered by kernel", true);
    row(s, "port", "CloveOS", "MagicX Mini Zero 28", true);
    row(s, "build", "Firmware build", SC_BUILD_ID, true);
    status(s, "B returns to Settings.");
}

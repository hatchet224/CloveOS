#ifdef NDEBUG
#error "Network regression requires assertions"
#endif
#include "../src/network.c"
#include <assert.h>
#include <sys/wait.h>
static bool media_ok = true;
enum cs_store_status cs_store_check_media(const struct cs_store *s)
{ (void)s; return media_ok ? CS_STORE_OK : CS_STORE_MEDIA; }
static void write_file(const char *name, const char *text)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0); assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}
static void fake_server(int fd, int log_fd)
{
    for (;;) {
        char input[1024]; struct sockaddr_un peer; socklen_t size = sizeof(peer);
        ssize_t count = recvfrom(fd, input, sizeof(input) - 1U, 0, (struct sockaddr *)&peer, &size);
        const char *reply = "OK\n";
        if (count < 1) _exit(2);
        input[count] = '\0';
        assert(write(log_fd, input, (size_t)count) == count);
        assert(write(log_fd, "\n", 1U) == 1);
        if (strcmp(input, "STATUS") == 0)
            reply = "wpa_state=COMPLETED\nip_address=192.0.2.9\n";
        else if (strcmp(input, "SCAN_RESULTS") == 0)
            reply = "bssid / frequency / signal level / flags / ssid\n"
                "01:02:03:04:05:06\t2412\t-48\t[WPA2-PSK-CCMP][ESS]\tHome\\x20Net\n"
                "01:02:03:04:05:07\t2412\t-50\t[WPA3-SAE-CCMP][ESS]\tWPA3 Only\n"
                "01:02:03:04:05:08\t2412\t-61\t[ESS]\tOpen;$(touch nope)\n"
                "malformed\t2412\t-60\t[ESS]\tBad\n";
        else if (strncmp(input, "BSS ", 4U) == 0)
            reply = "flags=[WPA2-PSK-CCMP][ESS]\nssid=Home\\x20Net\n";
        else if (strcmp(input, "ADD_NETWORK") == 0) reply = "7\n";
        else if (strcmp(input, "SAVE_CONFIG") == 0)
            write_file(WPA_CONFIG, "ctrl_interface=/run/console/wpa\nupdate_config=1\n# saved fixture\n");
        else if (strcmp(input, "STOPTEST") == 0) _exit(0);
        if (sendto(fd, reply, strlen(reply), 0, (struct sockaddr *)&peer, size) < 0) _exit(3);
    }
}
int main(int argc, char **argv)
{
    struct cs_store store = {0}; struct cl_tool_request request = {0};
    struct cl_tool_snapshot *snapshot = calloc(1U, sizeof(*snapshot));
    struct sockaddr_un address = {0}; struct connection control = {-1};
    unsigned char ssid[32]; size_t length; bool secured;
    char label[96], transcript[8192], saved[256];
    int fd, log_fd, result; pid_t child;
    assert(argc == 2 && snapshot != NULL);
    assert(ssid_decode("quote\\\"\\\\\\x20end", ssid, &length));
    assert(length == 11U); /* quote + quote + backslash + space + end */
    assert(!ssid_decode("bad\\x0q", ssid, &length));
    assert(!ssid_decode("123456789012345678901234567890123", ssid, &length));
    assert(security("[WPA2-PSK-CCMP][ESS]", &secured) && secured);
    assert(!security("[WEP][ESS]", &secured));
    assert(!security("[WPA3-SAE-CCMP][ESS]", &secured));
    assert(security("[ESS]", &secured) && !secured);
    ssid_label((const unsigned char *)"bad\nname", 8U, label); assert(strcmp(label, "bad?name") == 0);
    ssid_label((const unsigned char *)"a\0b", 3U, label); assert(strstr(label, "non-text") != NULL);
    store.card_fd = open(argv[1], O_RDONLY | O_DIRECTORY); assert(store.card_fd >= 0);
    assert(mkdirat(store.card_fd, ".console", 0700) == 0);
    store.data_fd = openat(store.card_fd, ".console", O_RDONLY | O_DIRECTORY); assert(store.data_fd >= 0);
    store.fat = true; store.qualified_fat_syncfs = true;
    request.operation = CL_TOOL_LOAD_NETWORK;
    network_at(&store, &request, snapshot, "/run/console/missing-socket");
    assert(snapshot->ready && snapshot->count == 1U && strstr(snapshot->status, "radio") != NULL);
    address.sun_family = AF_UNIX; strcpy(address.sun_path, "/run/console/test-wpa");
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); assert(fd >= 0);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    log_fd = open("/run/console/wpa-transcript", O_RDWR | O_CREAT | O_TRUNC, 0600); assert(log_fd >= 0);
    child = fork(); assert(child >= 0);
    if (child == 0) fake_server(fd, log_fd);
    close(fd);
    network_at(&store, &request, snapshot, address.sun_path);
    assert(snapshot->count == 7U && strstr(snapshot->status, "192.0.2.9") != NULL);
    assert(strcmp(snapshot->rows[4].label, "Home Net") == 0 && snapshot->rows[4].available && snapshot->rows[4].required);
    assert(!snapshot->rows[5].available && snapshot->rows[6].available);
    request.operation = CL_TOOL_CONNECT_NETWORK; request.enabled_mask = UINT64_C(0x010203040506);
    strcpy(request.id, "pa\"ss\\word");
    network_at(&store, &request, snapshot, address.sun_path);
    assert(strstr(snapshot->status, "Wi-Fi connected") != NULL &&
           strstr(snapshot->status, "192.0.2.9") != NULL);
    fd = openat(store.data_fd, "wifi.conf", O_RDONLY); assert(fd >= 0);
    ssize_t count = read(fd, saved, sizeof(saved) - 1U); assert(count > 0); saved[count] = '\0'; close(fd);
    assert(strstr(saved, "saved fixture") != NULL);
    media_ok = false;
    network_at(&store, &request, snapshot, address.sun_path);
    assert(strstr(snapshot->status, "not durably saved") != NULL); media_ok = true;
    memset(request.id, 'a', sizeof(request.id));
    network_at(&store, &request, snapshot, address.sun_path);
    assert(strstr(snapshot->status, "rejected") != NULL);
    assert(lseek(log_fd, 0, SEEK_SET) == 0); count = read(log_fd, transcript, sizeof(transcript) - 1U);
    assert(count > 0); transcript[count] = '\0';
    assert(strstr(transcript, "SET_NETWORK 7 ssid 486f6d65204e6574\n") != NULL);
    assert(strstr(transcript, "SET_NETWORK 7 psk \"pa\\\"ss\\\\word\"\n") != NULL);
    assert(strstr(transcript, "SET_NETWORK 7 proto RSN\n") != NULL);
    assert(connect_control(&control, address.sun_path));
    assert(send(control.fd, "STOPTEST", 8U, 0) == 8); close(control.fd);
    assert(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    close(log_fd); close(store.data_fd); close(store.card_fd); free(snapshot);
    puts("NETWORK_PASS native Unix-datagram IPC, scans, escaped SSIDs/credentials, WPA2 security gate, FAT atomic persistence, media failure, missing service");
    return 0;
}

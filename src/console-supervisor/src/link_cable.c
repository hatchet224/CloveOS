/* Temporary two-cartridge setup. The peer supplies battery bytes, never ROMs
 * or paths. Only the locally selected cartridge can be published on exit. */
#include "sessiond/sha256.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINK_PORT 55437U
#define ROM_LIMIT (8U * 1024U * 1024U)
#define SRAM_LIMIT (128U * 1024U)
#define RTC_BYTES 4U
#define PATH_CAP 1024U

struct link_header {
    char magic[8], proof[64], rom[64], data_hash[64];
    uint32_t sram, rtc;
};
_Static_assert(sizeof(struct link_header) == 208U, "link wire format");

struct cartridge {
    char path[PATH_CAP], hash[65];
    size_t sram;
};
struct battery {
    size_t sram, rtc;
    unsigned char data[SRAM_LIMIT + RTC_BYTES];
};

static bool read_exact(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    while (length != 0U) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        length -= (size_t)n;
    }
    return true;
}

static bool write_exact(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    while (length != 0U) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        length -= (size_t)n;
    }
    return true;
}

static bool record_read(int fd, const char *proof, char hash[65], struct battery *battery)
{
    struct link_header header;
    char checksum[65];
    if (!read_exact(fd, &header, sizeof(header)) || memcmp(header.magic, "CLVLINK1", 8U) != 0 ||
        (proof != NULL && memcmp(proof, header.proof, 64U) != 0)) return false;
    memcpy(hash, header.rom, 64U);
    hash[64] = '\0';
    battery->sram = ntohl(header.sram);
    battery->rtc = ntohl(header.rtc);
    size_t length = battery->sram + battery->rtc;
    if (!cs_hex_valid(hash, 64U) || battery->sram > SRAM_LIMIT ||
        (battery->rtc != 0U && battery->rtc != RTC_BYTES) || !read_exact(fd, battery->data, length)) return false;
    cs_sha256_hex(battery->data, length, checksum);
    return memcmp(checksum, header.data_hash, 64U) == 0;
}

static bool record_write(int fd, const char *proof, const char hash[65], const struct battery *battery)
{
    struct link_header header = {.magic = "CLVLINK1", .sram = htonl((uint32_t)battery->sram),
                                 .rtc = htonl((uint32_t)battery->rtc)};
    char checksum[65];
    memcpy(header.rom, hash, 64U);
    if (proof != NULL) memcpy(header.proof, proof, 64U);
    size_t length = battery->sram + battery->rtc;
    cs_sha256_hex(battery->data, length, checksum);
    memcpy(header.data_hash, checksum, 64U);
    return write_exact(fd, &header, sizeof(header)) && write_exact(fd, battery->data, length);
}

static bool extension_valid(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL && (strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0);
}

static bool cartridge_read(const char *path, struct cartridge *cart, size_t *budget)
{
    static const unsigned int ram_banks[] = {0, 1, 1, 4, 16, 8};
    struct stat info;
    unsigned char buffer[32768];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = extension_valid(path) && strlen(path) < sizeof(cart->path) &&
              fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= (off_t)sizeof(buffer) &&
              (uint64_t)info.st_size <= ROM_LIMIT && (uint64_t)info.st_size <= *budget &&
              read_exact(fd, buffer, sizeof(buffer));
    if (ok) {
        unsigned int ram_code = buffer[0x149];
        unsigned char checksum = 0;
        for (size_t i = 0x134; i < 0x14d; ++i) checksum = (unsigned char)(checksum - buffer[i] - 1U);
        ok = ram_code < sizeof(ram_banks) / sizeof(*ram_banks) && buffer[0x14d] == checksum;
        *budget -= (size_t)info.st_size;
        if (ok) {
            struct cs_sha256 digest;
            cs_sha256_init(&digest);
            cs_sha256_update(&digest, buffer, sizeof(buffer));
            size_t remaining = (size_t)info.st_size - sizeof(buffer);
            while (remaining > 0U) {
                size_t part = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
                if (!read_exact(fd, buffer, part)) { ok = false; break; }
                cs_sha256_update(&digest, buffer, part);
                remaining -= part;
            }
            cs_sha256_final(&digest, cart->hash);
            strcpy(cart->path, path);
            cart->sram = ram_banks[ram_code] * 8192U;
        }
    }
    close(fd);
    return ok;
}

static bool find_rom(const char *folder, const char hash[65], struct cartridge *cart,
                     unsigned int depth, size_t *budget, unsigned int *remaining)
{
    int fd = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *directory = fdopendir(fd);
    if (directory == NULL) { close(fd); return false; }
    bool found = false;
    for (struct dirent *entry; *remaining != 0U && (entry = readdir(directory)) != NULL;) {
        if (entry->d_name[0] == '.') continue;
        --*remaining;
        char path[PATH_CAP];
        int length = snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        if (extension_valid(path)) {
            if (cartridge_read(path, cart, budget) && strcmp(hash, cart->hash) == 0) { found = true; break; }
        } else if (depth > 0U && find_rom(path, hash, cart, depth - 1U, budget, remaining)) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

static int save_directory(const char *card, bool create)
{
    int root = open(card, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -1;
    if (create && mkdirat(root, "Saves", 0700) != 0 && errno != EEXIST) { close(root); return -1; }
    int parent = openat(root, "Saves", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int error = errno;
    close(root);
    if (parent < 0) { errno = error; return -1; }
    if (create && mkdirat(parent, "LinkCable", 0700) != 0 && errno != EEXIST) { close(parent); return -1; }
    int directory = openat(parent, "LinkCable", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    error = errno;
    close(parent);
    errno = error;
    return directory;
}

static bool save_read(int directory, const struct cartridge *cart, struct battery *battery)
{
    char name[80], hash[65], extra;
    (void)snprintf(name, sizeof(name), "%s.link", cart->hash);
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    struct stat info;
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && record_read(fd, NULL, hash, battery) &&
              strcmp(hash, cart->hash) == 0 && battery->sram == cart->sram && read(fd, &extra, 1) == 0;
    close(fd);
    return ok;
}

static bool publish(int directory, const char *name, const char hash[65], const struct battery *battery)
{
    char temporary[64];
    (void)snprintf(temporary, sizeof(temporary), ".link-%ld.new", (long)getpid());
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = record_write(fd, NULL, hash, battery) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = renameat(directory, temporary, directory, name) == 0 && syncfs(directory) == 0;
    if (!ok) (void)unlinkat(directory, temporary, 0);
    return ok;
}

static bool slot_file(const char *dir, unsigned int slot, const char *extension,
                      unsigned char *buffer, size_t size, bool create)
{
    char path[PATH_CAP];
    int length = snprintf(path, sizeof(path), "%s/player%u.%s", dir, slot, extension);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    int flags = create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY | O_NONBLOCK;
    int fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    struct stat info;
    bool ok = create ? write_exact(fd, buffer, size) : fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
              (uint64_t)info.st_size == size && read_exact(fd, buffer, size);
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool stage(const char *root, const char *saves, unsigned int slot,
                  const struct cartridge *cart, struct battery *battery)
{
    char path[PATH_CAP];
    int length = snprintf(path, sizeof(path), "%s/player%u.gb", root, slot);
    if (length < 0 || (size_t)length >= sizeof(path) || symlink(cart->path, path) != 0) return false;
    return (battery->sram == 0U || slot_file(saves, slot, "srm", battery->data, battery->sram, true)) &&
           (battery->rtc == 0U || slot_file(saves, slot, "rtc", battery->data + battery->sram, battery->rtc, true));
}

static int exchange(bool host, const char *address, const char *proof, const char *card,
                    const char *directory, struct cartridge *own, struct battery *local)
{
    struct sockaddr_in endpoint = {.sin_family = AF_INET, .sin_port = htons(LINK_PORT)};
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1) return 2;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 22;
    if (host) {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
            bind(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) != 0 || listen(fd, 1) != 0) return 22;
        alarm(120);
        int peer = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        close(fd);
        if (peer < 0) return 22;
        fd = peer;
    } else {
        alarm(5);
        if (connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) != 0) return 20;
    }
    alarm(25);
    char peer_hash[65];
    struct battery remote = {0};
    bool ok = host ? record_read(fd, proof, peer_hash, &remote) && record_write(fd, proof, own->hash, local) :
                     record_write(fd, proof, own->hash, local) && record_read(fd, proof, peer_hash, &remote);
    if (!ok) return 21;
    struct cartridge other;
    bool found = strcmp(own->hash, peer_hash) == 0;
    if (found) other = *own;
    size_t budget = 256U * 1024U * 1024U;
    unsigned int entries = 2048U;
    for (unsigned int system = 0; !found && system < 2U; ++system) {
        char path[PATH_CAP];
        int length = snprintf(path, sizeof(path), "%s/Roms/%s", card, system == 0U ? "GB" : "GBC");
        if (length > 0 && (size_t)length < sizeof(path)) found = find_rom(path, peer_hash, &other, 2U, &budget, &entries);
    }
    ok = found && (remote.sram == 0U || remote.sram == other.sram);
    unsigned char ready = ok ? 1U : 0U, peer_ready = 0U;
    if (!write_exact(fd, &ready, 1U) || !read_exact(fd, &peer_ready, 1U) || !ok || peer_ready != 1U) return 24;
    close(fd);
    alarm(0);
    char saves[PATH_CAP];
    int length = snprintf(saves, sizeof(saves), "%s/%s", directory, host ? "saves" : "saves/.netplay");
    if (length < 0 || (size_t)length >= sizeof(saves) ||
        !stage(directory, saves, host ? 1U : 2U, own, local) ||
        !stage(directory, saves, host ? 2U : 1U, &other, &remote)) return 25;
    return 0;
}

int main(int argc, char **argv)
{
    bool commit = argc == 7 && strcmp(argv[1], "commit") == 0;
    bool prepare = argc == 9 && strcmp(argv[1], "prepare") == 0;
    if (!commit && !prepare) return 2;
    bool host = strcmp(argv[2], "host") == 0;
    if (!host && strcmp(argv[2], "join") != 0) return 2;
    const char *card = argv[3], *content = argv[4], *hash = argv[5], *directory = argv[6];
    if (!cs_hex_valid(hash, 64U) || (prepare && !cs_hex_valid(argv[8], 64U))) return 2;
    struct cartridge own;
    size_t budget = ROM_LIMIT;
    if (!cartridge_read(content, &own, &budget) || strcmp(hash, own.hash) != 0 || own.sram == 0U) return 21;
    struct battery local = {0};
    int persistent = save_directory(card, commit);
    if (persistent < 0 ? (commit || errno != ENOENT) : !save_read(persistent, &own, &local)) return 25;
    if (prepare) {
        if (persistent >= 0) close(persistent);
        return exchange(host, argv[7], argv[8], card, directory, &own, &local);
    }
    char saves[PATH_CAP], name[80];
    int length = snprintf(saves, sizeof(saves), "%s/%s", directory, host ? "saves" : "saves/.netplay");
    struct battery updated = {.sram = own.sram, .rtc = RTC_BYTES};
    unsigned int slot = host ? 1U : 2U;
    if (length < 0 || (size_t)length >= sizeof(saves) ||
        !slot_file(saves, slot, "srm", updated.data, updated.sram, false) ||
        !slot_file(saves, slot, "rtc", updated.data + updated.sram, updated.rtc, false)) return 25;
    if (local.sram != 0U && (local.rtc != updated.rtc ||
        memcmp(local.data, updated.data, updated.sram + updated.rtc) != 0)) {
        (void)snprintf(name, sizeof(name), "%s.previous", own.hash);
        if (!publish(persistent, name, own.hash, &local)) return 25;
    }
    (void)snprintf(name, sizeof(name), "%s.link", own.hash);
    bool ok = publish(persistent, name, own.hash, &updated);
    close(persistent);
    return ok ? 0 : 25;
}

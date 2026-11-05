#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "layout.h"

#define SECTOR 512U
#define TABLE_BYTES 16384U
#define FAT_LBA UINT64_C(2097152)
#define FAT_SECTORS 262144U
#define CLUSTER_SECTORS 64U
#define MAX_BYTES (UINT64_C(1) << 40)
#define MIN_BYTES (UINT64_C(4) << 30)
#define JOURNAL (UINT64_C(768) << 20)
#define SLOT_BYTES 65536U
#define PLAN_BYTES 19456U
#define PMBR 512U
#define PPRIMARY 1024U
#define PTABLE 1536U
#define PBACKUP 17920U
#define PBOOT 18432U
#define PINFO 18944U

static const unsigned char journal_magic[16] = "CLOVE-GROW-v1";
static const unsigned char format_magic[24] = "CloveOS-grow-FAT32-v1";
static unsigned int write_step;
#ifdef AUTOGROW_TESTING
static unsigned int stop_step;
static bool tear_write;
#endif

static void fail(const char *message) {
    fprintf(stderr, "CloveOS storage expansion: %s (%s)\n", message, strerror(errno));
    exit(1);
}
static void check(bool condition, const char *message) {
    if (!condition) { errno = EINVAL; fail(message); }
}
static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static void put32(unsigned char *p, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static void put64(unsigned char *p, uint64_t value) {
    put32(p, (uint32_t)value); put32(p + 4, (uint32_t)(value >> 32));
}
static uint32_t crc(const unsigned char *bytes, size_t length) {
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        value ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (UINT32_C(0xedb88320) & (0U - (value & 1U)));
    }
    return ~value;
}
static bool read_at(int fd, void *buffer, size_t length, uint64_t offset) {
    unsigned char *out = buffer;
    while (length) {
        ssize_t count = pread(fd, out, length, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        out += (size_t)count; length -= (size_t)count; offset += (uint64_t)count;
    }
    return true;
}
static void write_at(int fd, const void *buffer, size_t length, uint64_t offset) {
    const unsigned char *in = buffer;
#ifdef AUTOGROW_TESTING
    if (tear_write && stop_step == write_step + 1) {
        size_t partial = length / 2;
        check(pwrite(fd, in, partial, (off_t)offset) == (ssize_t)partial, "injecting a torn metadata write");
        fsync(fd); _exit(86);
    }
#endif
    while (length) {
        ssize_t count = pwrite(fd, in, length, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail("writing storage metadata");
        in += (size_t)count; length -= (size_t)count; offset += (uint64_t)count;
    }
    if (fsync(fd) != 0) fail("flushing storage metadata");
    ++write_step;
#ifdef AUTOGROW_TESTING
    if (stop_step && write_step == stop_step) _exit(86);
#endif
}
static void header_crc(unsigned char *header) {
    put32(header + 16, 0); put32(header + 16, crc(header, 92));
}
static bool header_valid(const unsigned char *header) {
    unsigned char copy[512]; memcpy(copy, header, sizeof(copy));
    if (memcmp(header, "EFI PART", 8) || u32(header + 12) != 92) return false;
    header_crc(copy); return u32(copy + 16) == u32(header + 16);
}
static bool all_zero(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) if (bytes[i]) return false;
    return true;
}
static bool plan_valid(const unsigned char *plan) {
    unsigned char header[512]; memcpy(header, plan, 512); put32(header + 56, 0);
    return !memcmp(plan, journal_magic, 16) && u32(plan + 16) == 1 &&
        crc(header, 512) == u32(plan + 56) && crc(plan + 512, PLAN_BYTES - 512) == u32(plan + 52) &&
        crc(plan + PTABLE, 9 * 128) == STOCK_PARTITIONS_CRC &&
        header_valid(plan + PPRIMARY) && header_valid(plan + PBACKUP) &&
        !memcmp(plan + PPRIMARY + 56, plan + 80, 16) &&
        u64(plan + 32) >= MIN_BYTES && u64(plan + 32) <= MAX_BYTES;
}
static uint32_t clusters(uint32_t total) {
    uint32_t overhead = 32 + 2 * FAT_SECTORS;
    check(total > overhead, "invalid FAT32 volume bounds");
    uint32_t count = (total - overhead) / CLUSTER_SECTORS;
    check(count >= 65525 && (uint64_t)count + 2 <= (uint64_t)FAT_SECTORS * 128, "unsupported FAT32 cluster count");
    return count;
}
static bool boot_geometry(const unsigned char *boot) {
    return u16(boot + 11) == 512 && boot[13] == CLUSTER_SECTORS && u16(boot + 14) == 32 &&
        boot[16] == 2 && u16(boot + 17) == 0 && u16(boot + 19) == 0 && u16(boot + 22) == 0 &&
        u32(boot + 28) == FAT_LBA && u32(boot + 36) == FAT_SECTORS && u16(boot + 40) == 0 &&
        u32(boot + 44) == 2 && u16(boot + 48) == 1 && u16(boot + 50) == 6 &&
        u32(boot + 67) == UINT32_C(0xc0280002) && boot[510] == 0x55 && boot[511] == 0xaa;
}
static bool read_match(int fd, const unsigned char *expected, size_t size, uint64_t offset) {
    unsigned char buffer[TABLE_BYTES];
    check(size <= sizeof(buffer), "oversized metadata comparison");
    return read_at(fd, buffer, size, offset) && !memcmp(buffer, expected, size);
}
static bool complete(int fd, const unsigned char *plan) {
    uint64_t bytes = u64(plan + 32), final = bytes / 512 - 1;
    unsigned char boot[512], backup[512], mbr[512];
    return read_at(fd, boot, 512, FAT_LBA * 512) && boot_geometry(boot) &&
        u32(boot + 32) == u32(plan + PBOOT + 32) &&
        read_at(fd, backup, 512, FAT_LBA * 512 + 6 * 512) && boot_geometry(backup) &&
        u32(backup + 32) == u32(boot + 32) &&
        read_at(fd, mbr, 512, 0) && !memcmp(mbr + 446, plan + PMBR + 446, 64) &&
        read_match(fd, plan + PPRIMARY, 512, 512) && read_match(fd, plan + PTABLE, TABLE_BYTES, 2048 * 512) &&
        read_match(fd, plan + PTABLE, TABLE_BYTES, (final - 32) * 512) &&
        read_match(fd, plan + PBACKUP, 512, final * 512);
}
static void free_clusters(int fd, uint32_t old_total, uint32_t new_total, unsigned char *info) {
    const size_t chunk = 1024 * 1024;
    unsigned char *a = malloc(chunk), *b = malloc(chunk);
    check(a && b, "allocating FAT validation buffers");
    uint32_t old_count = clusters(old_total), new_count = clusters(new_total), free_count = 0, first = 0;
    for (uint64_t pos = 0; pos < (uint64_t)FAT_SECTORS * 512; pos += chunk) {
        check(read_at(fd, a, chunk, FAT_LBA * 512 + 32 * 512 + pos) &&
              read_at(fd, b, chunk, FAT_LBA * 512 + (32 + (uint64_t)FAT_SECTORS) * 512 + pos), "reading both FAT tables");
        check(!memcmp(a, b, chunk), "FAT tables disagree; filesystem repair is required");
        if (!pos) check((u32(a + 4) & UINT32_C(0x0c000000)) == UINT32_C(0x0c000000),
                        "FAT32 is dirty or reports I/O errors; repair it before expansion");
        for (size_t index = 0; index < chunk / 4; ++index) {
            uint64_t number = pos / 4 + index;
            uint32_t value = u32(a + index * 4) & UINT32_C(0x0fffffff);
            if (number >= (uint64_t)old_count + 2) check(value == 0, "preallocated FAT region is occupied");
            if (number >= 2 && number < (uint64_t)new_count + 2 && !value) {
                ++free_count; if (!first) first = (uint32_t)number;
            }
        }
    }
    free(a); free(b);
    put32(info + 488, free_count); put32(info + 492, first ? first : UINT32_MAX);
}
static void make_plan(int fd, uint64_t bytes, uint64_t sequence, unsigned char *plan) {
    unsigned char marker[512];
    memset(plan, 0, PLAN_BYTES);
    memcpy(plan, journal_magic, 16); put32(plan + 16, 1); put64(plan + 24, sequence); put64(plan + 32, bytes);
    check(read_at(fd, plan + PMBR, 512, 0) && read_at(fd, plan + PPRIMARY, 512, 512) &&
          read_at(fd, plan + PTABLE, TABLE_BYTES, 2048 * 512) &&
          read_at(fd, plan + PBOOT, 512, FAT_LBA * 512) && read_at(fd, plan + PINFO, 512, FAT_LBA * 512 + 512) &&
          read_at(fd, marker, 512, FAT_LBA * 512 + 1024), "reading original capacity metadata");
    unsigned char *header = plan + PPRIMARY, *table = plan + PTABLE, *boot = plan + PBOOT;
    check(!(boot[65] & 1U), "FAT32 boot-sector dirty flag is set; repair it before expansion");
    check(header_valid(header) && u64(header + 24) == 1 && u64(header + 72) == 2048 &&
          u32(header + 80) == 128 && u32(header + 84) == 128 && crc(table, TABLE_BYTES) == u32(header + 88), "GPT is damaged or unsupported");
    check(crc(table, 9 * 128) == STOCK_PARTITIONS_CRC && all_zero(table + 10 * 128, TABLE_BYTES - 10 * 128), "unexpected system partitions");
    check(u64(table + 9 * 128 + 32) == FAT_LBA && boot_geometry(boot) &&
          !memcmp(marker, format_magic, 24) && u32(marker + 24) == FAT_SECTORS && u32(marker + 28) == CLUSTER_SECTORS,
          "this volume does not use the CloveOS expandable format");
    uint64_t old_backup = u64(header + 32), final = bytes / 512 - 1, last = final - 33;
    uint32_t old_total = u32(boot + 32), total = (uint32_t)(last - FAT_LBA + 1);
    check(bytes % 512 == 0 && last >= FAT_LBA && last - FAT_LBA + 1 <= UINT32_MAX && total >= old_total &&
          u64(table + 9 * 128 + 40) + 1 == old_backup - 32, "invalid old or new data partition extent");
    check(old_backup * 512 < bytes && (uint64_t)old_total <= old_backup - 32 - FAT_LBA, "card shrank or old filesystem overlaps metadata");
    put64(plan + 40, old_backup); put32(plan + 64, old_total); memcpy(plan + 80, header + 56, 16);
    check(u32(plan + PINFO) == UINT32_C(0x41615252) && u32(plan + PINFO + 484) == UINT32_C(0x61417272), "invalid FAT32 FSInfo");
    fprintf(stderr, "CloveOS: checking FAT tables before expanding to %" PRIu64 " bytes\n", bytes);
    free_clusters(fd, old_total, total, plan + PINFO);
    put32(boot + 32, total); put64(table + 9 * 128 + 40, last);
    put64(header + 32, final); put64(header + 48, last); put32(header + 88, crc(table, TABLE_BYTES)); header_crc(header);
    memcpy(plan + PBACKUP, header, 512); put64(plan + PBACKUP + 24, final); put64(plan + PBACKUP + 32, 1);
    put64(plan + PBACKUP + 72, final - 32); header_crc(plan + PBACKUP);
    check(plan[PMBR + 450] == 0x0c && u32(plan + PMBR + 454) == FAT_LBA &&
          plan[PMBR + 466] == 0x83 && plan[PMBR + 482] == 0xee, "unexpected hybrid MBR");
    put32(plan + PMBR + 458, total);
    put32(plan + 52, crc(plan + 512, PLAN_BYTES - 512)); put32(plan + 56, 0); put32(plan + 56, crc(plan, 512));
    check(plan_valid(plan), "invalid generated expansion transaction");
}
static void journal_plan(int fd, const unsigned char *plan, unsigned int slot) {
    uint64_t offset = JOURNAL + (uint64_t)slot * SLOT_BYTES;
    unsigned char *verify = malloc(PLAN_BYTES);
    check(verify != NULL, "allocating journal verification");
    write_at(fd, plan + 512, PLAN_BYTES - 512, offset + 512);
    check(read_at(fd, verify + 512, PLAN_BYTES - 512, offset + 512) && !memcmp(verify + 512, plan + 512, PLAN_BYTES - 512), "journal payload readback failed");
    write_at(fd, plan, 512, offset);
    check(read_at(fd, verify, PLAN_BYTES, offset) && plan_valid(verify) && !memcmp(verify, plan, PLAN_BYTES), "journal commit readback failed");
    free(verify);
}
static void apply(int fd, const unsigned char *plan) {
    uint64_t final = u64(plan + 32) / 512 - 1, old = u64(plan + 40);
    unsigned char boot[512], zeros[33 * 512] = {0};
    check(read_at(fd, boot, 512, FAT_LBA * 512), "reading filesystem commit marker");
    check(!(boot[65] & 1U), "FAT32 boot-sector dirty flag is set; repair it before metadata recovery");
    bool committed = boot_geometry(boot) && u32(boot + 32) == u32(plan + PBOOT + 32);
    fprintf(stderr, "CloveOS: replaying metadata; filesystem_committed=%u\n", committed ? 1U : 0U);
    unsigned char info[512];
    memcpy(info, plan + PINFO, sizeof(info));
    if (committed) free_clusters(fd, u32(boot + 32), u32(boot + 32), info);
    write_at(fd, plan + PTABLE, TABLE_BYTES, (final - 32) * 512);
    write_at(fd, plan + PBACKUP, 512, final * 512);
    write_at(fd, plan + PTABLE, TABLE_BYTES, 2048 * 512);
    write_at(fd, plan + PPRIMARY, 512, 512);
    write_at(fd, plan + PMBR, 512, 0);
    if (!committed && old != final) write_at(fd, zeros, sizeof(zeros), (old - 32) * 512);
    write_at(fd, plan + PBOOT, 512, FAT_LBA * 512 + 6 * 512);
    write_at(fd, info, 512, FAT_LBA * 512 + 7 * 512);
    write_at(fd, info, 512, FAT_LBA * 512 + 512);
    write_at(fd, plan + PBOOT, 512, FAT_LBA * 512);
    check(complete(fd, plan), "expanded metadata readback failed");
}
static bool is_mounted(dev_t device, const char *filesystem) {
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) fail("reading mount ownership");
    char *line = NULL; size_t size = 0; bool found = false;
    while (getline(&line, &size, mounts) > 0) {
        unsigned int a, b;
        char *kind = strstr(line, " - ");
        if (sscanf(line, "%*u %*u %u:%u", &a, &b) == 2 && makedev(a, b) == device &&
            (!filesystem || (kind && !strncmp(kind + 3, filesystem, strlen(filesystem)) && kind[3 + strlen(filesystem)] == ' '))) found = true;
    }
    free(line); fclose(mounts); return found;
}
static bool auto_device(char disk[256], char partition[256]) {
    DIR *entries = opendir("/sys/class/block");
    if (!entries) fail("reading block-device inventory");
    struct dirent *entry; unsigned int found = 0;
    while ((entry = readdir(entries))) {
        if (entry->d_name[0] == '.' || strlen(entry->d_name) > 100) continue;
        char attribute[256], root_path[256], root_name[128];
        snprintf(attribute, sizeof(attribute), "/sys/class/block/%s/partition", entry->d_name);
        if (access(attribute, F_OK) == 0) continue;
        size_t n = strlen(entry->d_name);
        const char *separator = entry->d_name[n - 1] >= '0' && entry->d_name[n - 1] <= '9' ? "p" : "";
        snprintf(root_name, sizeof(root_name), "%s%s5", entry->d_name, separator);
        snprintf(root_path, sizeof(root_path), "/dev/%s", root_name);
        struct stat root_stat;
        if (stat(root_path, &root_stat) || !S_ISBLK(root_stat.st_mode) || !is_mounted(root_stat.st_rdev, "squashfs")) continue;
        snprintf(disk, 256, "/dev/%s", entry->d_name);
        snprintf(partition, 256, "/dev/%s%s10", entry->d_name, separator);
        ++found;
    }
    closedir(entries); return found == 1;
}
int main(int argc, char **argv) {
    char disk[256] = "", partition[256] = "";
    check(argc >= 2 && !strcmp(argv[1], "--auto"), "usage: console-grow --auto");
#ifdef AUTOGROW_TESTING
    if (argc == 4 && (!strcmp(argv[2], "--cut") || !strcmp(argv[2], "--tear"))) {
        stop_step = (unsigned int)strtoul(argv[3], NULL, 10); tear_write = !strcmp(argv[2], "--tear");
    }
    else check(argc == 2, "invalid test arguments");
#else
    check(argc == 2, "invalid arguments");
#endif
    check(auto_device(disk, partition), "cannot uniquely identify the running system's primary card");
    struct stat data_stat, disk_stat;
    check(!stat(partition, &data_stat) && S_ISBLK(data_stat.st_mode) && !is_mounted(data_stat.st_rdev, NULL), "data volume must be offline");
    int fd = open(disk, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &disk_stat) || !S_ISBLK(disk_stat.st_mode)) fail("opening the primary card");
    if (flock(fd, LOCK_EX | LOCK_NB)) fail("another expansion is active");
    /* Parent and partition devices have distinct page caches. Drop both while
       FAT is offline so metadata replay cannot read pre-unmount allocation data. */
    int offline = open(partition, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (offline < 0 || ioctl(offline, BLKFLSBUF) || ioctl(fd, BLKFLSBUF))
        fail("flushing offline filesystem metadata before expansion");
    close(offline);
    uint64_t bytes = 0; int logical = 0;
    if (ioctl(fd, (int)BLKGETSIZE64, &bytes) || ioctl(fd, BLKSSZGET, &logical)) fail("reading actual card capacity");
    check(logical == 512 && bytes >= MIN_BYTES && bytes <= MAX_BYTES && bytes % 512 == 0, "card capacity or sector size is unsupported");
    unsigned char *a = calloc(1, PLAN_BYTES), *b = calloc(1, PLAN_BYTES), *plan = calloc(1, PLAN_BYTES);
    check(a && b && plan, "allocating expansion metadata");
    bool va = read_at(fd, a, PLAN_BYTES, JOURNAL) && plan_valid(a);
    bool vb = read_at(fd, b, PLAN_BYTES, JOURNAL + SLOT_BYTES) && plan_valid(b);
    unsigned char *latest = va && (!vb || u64(a + 24) >= u64(b + 24)) ? a : (vb ? b : NULL);
    uint64_t sequence = latest ? u64(latest + 24) : 0;
    if (latest) {
        check(u64(latest + 32) <= bytes, "card is smaller than its last committed expansion");
        if (!complete(fd, latest)) {
            fprintf(stderr, "CloveOS: recovering an interrupted storage expansion\n");
            apply(fd, latest);
        }
    }
    if (!latest || u64(latest + 32) != bytes) {
        make_plan(fd, bytes, sequence + 1, plan);
        unsigned int slot = latest == a ? 1 : 0;
        journal_plan(fd, plan, slot); apply(fd, plan); latest = plan;
    }
    struct blkpg_partition change; memset(&change, 0, sizeof(change));
    change.pno = 10; change.start = (long long)(FAT_LBA * 512);
    change.length = (long long)((uint64_t)u32(latest + PBOOT + 32) * 512);
    struct blkpg_ioctl_arg resize = {BLKPG_RESIZE_PARTITION, 0, (int)sizeof(change), &change};
    if (ioctl(fd, BLKPG, &resize)) fail("refreshing the kernel data-partition size; retry startup");
    int child = open(partition, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (child < 0 || ioctl(fd, BLKFLSBUF) || ioctl(child, BLKFLSBUF))
        fail("invalidating metadata caches before mounting the expanded filesystem");
    uint64_t child_bytes = 0;
    check(child >= 0 && !ioctl(child, (int)BLKGETSIZE64, &child_bytes) && child_bytes == (uint64_t)change.length,
          "kernel data-partition readback disagrees");
    close(child); close(fd); free(a); free(b); free(plan);
    fprintf(stderr, "CloveOS: full card ready; capacity=%" PRIu64 " data=%" PRIu64 " metadata_writes=%u\n", bytes, child_bytes, write_step);
    return 0;
}

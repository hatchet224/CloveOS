#include "supervisor/mount.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

static bool decode(const char *input, char out[4096])
{
    size_t i = 0U, j = 0U;
    while (input[i] != '\0' && j + 1U < 4096U) {
        if (input[i] == '\\') {
            unsigned int value;
            if (strlen(input + i) < 4U || input[i + 1U] < '0' || input[i + 1U] > '7' ||
                input[i + 2U] < '0' || input[i + 2U] > '7' || input[i + 3U] < '0' ||
                input[i + 3U] > '7') {
                return false;
            }
            value = (unsigned int)(input[i + 1U] - '0') * 64U +
                    (unsigned int)(input[i + 2U] - '0') * 8U + (unsigned int)(input[i + 3U] - '0');
            if (value == 0U || value > 255U) {
                return false;
            }
            out[j++] = (char)value;
            i += 4U;
        } else {
            out[j++] = input[i++];
        }
    }
    out[j] = '\0';
    return input[i] == '\0';
}
bool sc_mount_check(const char *path, const char *device, const char *uuid, const char *type,
                    bool read_only)
{
    char alias[160], info[64], line[8192], decoded[4096];
    struct stat target, block, alias_stat;
    struct statvfs vfs;
    FILE *file;
    int fd;
    unsigned long long mount_id = 0U;
    bool found = false, ok = true;
    if (path[0] != '/' || device[0] != '/' || strlen(uuid) > 64U || strchr(uuid, '/') != NULL) {
        return false;
    }
    (void)snprintf(alias, sizeof(alias), "/dev/disk/by-uuid/%s", uuid);
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &target) != 0 || stat(device, &block) != 0 || !S_ISBLK(block.st_mode) ||
        stat(alias, &alias_stat) != 0 || !S_ISBLK(alias_stat.st_mode) ||
        alias_stat.st_rdev != block.st_rdev || target.st_dev != block.st_rdev ||
        fstatvfs(fd, &vfs) != 0 || ((vfs.f_flag & ST_RDONLY) != 0U) != read_only) {
        ok = false;
    }
    (void)snprintf(info, sizeof(info), "/proc/self/fdinfo/%d", fd);
    file = fopen(info, "r");
    if (file == NULL) {
        ok = false;
    } else {
        while (fgets(line, sizeof(line), file) != NULL) {
            if (sscanf(line, "mnt_id: %llu", &mount_id) == 1) {
                break;
            }
        }
        if (ferror(file) != 0) {
            ok = false;
        }
        if (fclose(file) != 0) {
            ok = false;
        }
    }
    file = fopen("/proc/self/mountinfo", "r");
    if (file == NULL) {
        ok = false;
    } else {
        while (ok && fgets(line, sizeof(line), file) != NULL) {
            char *save = NULL, *token = strtok_r(line, " ", &save);
            unsigned long long id;
            unsigned int field;
            if (token == NULL || sscanf(token, "%llu", &id) != 1 || id != mount_id) {
                continue;
            }
            for (field = 0U; field < 4U && token != NULL; ++field) {
                token = strtok_r(NULL, " ", &save);
            }
            if (token == NULL || !decode(token, decoded) || strcmp(decoded, path) != 0) {
                break;
            }
            while (token != NULL && strcmp(token, "-") != 0) {
                token = strtok_r(NULL, " ", &save);
            }
            token = token == NULL ? NULL : strtok_r(NULL, " ", &save);
            found = token != NULL && strcmp(token, type) == 0;
            break;
        }
        if (ferror(file) != 0) {
            ok = false;
        }
        if (fclose(file) != 0) {
            ok = false;
        }
    }
    if (close(fd) != 0) {
        ok = false;
    }
    return ok && found;
}
bool sc_runtime_check(void)
{
    struct statfs fs;
    struct stat st;
    return statfs("/run", &fs) == 0 && (unsigned long)fs.f_type == 0x01021994UL &&
           lstat("/run/console", &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0U &&
           (st.st_mode & 0077U) == 0U;
}

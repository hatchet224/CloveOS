#include "supervisor/cpu_policy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static bool read_value(const char *root, const char *name, char *out, size_t cap)
{
    char path[640];
    int n = snprintf(path, sizeof(path), "%s/%s", root, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t size;
    do { size = read(fd, out, cap - 1U); } while (size < 0 && errno == EINTR);
    bool ok = close(fd) == 0 && size > 0 && (size_t)size < cap - 1U;
    if (ok) { out[size] = '\0'; out[strcspn(out, "\r\n")] = '\0'; }
    return ok;
}
static bool write_value(const char *root, const char *name, const char *value)
{
    char path[640];
    int n = snprintf(path, sizeof(path), "%s/%s", root, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    size_t length = strlen(value), used = 0U;
    while (used < length) {
        ssize_t wrote = write(fd, value + used, length - used);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        used += (size_t)wrote;
    }
    return close(fd) == 0 && used == length;
}
bool sc_cpu_policy_prepare(struct sc_cpu_policy *policy, const char *root)
{
    char available[512], copy[512], *token, *state = NULL;
    bool found = false;
    if (policy->changed || strlen(root) >= sizeof(policy->root)) return false;
    if (!read_value(root, "scaling_available_governors", available, sizeof(available)) ||
        !read_value(root, "scaling_governor", policy->governor, sizeof(policy->governor))) return false;
    strcpy(copy, available);
    for (token = strtok_r(copy, " \t", &state); token != NULL; token = strtok_r(NULL, " \t", &state))
        if (strcmp(token, "performance") == 0) found = true;
    if (!found) return false;
    if (strcmp(policy->governor, "performance") == 0) return true;
    strcpy(policy->root, root); policy->speed[0] = '\0';
    if (strcmp(policy->governor, "userspace") == 0)
        (void)read_value(root, "scaling_setspeed", policy->speed, sizeof(policy->speed));
    if (!write_value(root, "scaling_governor", "performance")) return false;
    policy->changed = true;
    char actual[64], maximum[64] = "unknown";
    (void)read_value(root, "scaling_max_freq", maximum, sizeof(maximum));
    bool ok = read_value(root, "scaling_governor", actual, sizeof(actual)) && strcmp(actual, "performance") == 0;
    fprintf(stderr, "CPU policy: %s -> performance, kernel maximum=%s kHz verified=%u\n",
            policy->governor, maximum, (unsigned int)ok);
    return ok;
}
bool sc_cpu_policy_restore(struct sc_cpu_policy *policy)
{
    if (!policy->changed) return true;
    if (!write_value(policy->root, "scaling_governor", policy->governor)) return false;
    if (strcmp(policy->governor, "userspace") == 0 && policy->speed[0] != '\0' &&
        strspn(policy->speed, "0123456789") == strlen(policy->speed) &&
        !write_value(policy->root, "scaling_setspeed", policy->speed)) return false;
    policy->changed = false;
    fprintf(stderr, "CPU policy: restored %s after emulator exit\n", policy->governor);
    return true;
}
bool sc_cpu_policy_idle(const char *root)
{
    char current[64], available[512], *part, *state = NULL;
    if (!read_value(root, "scaling_governor", current, sizeof(current)) ||
        strcmp(current, "performance") != 0 ||
        !read_value(root, "scaling_available_governors", available, sizeof(available))) return false;
    for (part = strtok_r(available, " \t", &state); part != NULL; part = strtok_r(NULL, " \t", &state))
        if (strcmp(part, "schedutil") == 0) break;
    if (part == NULL || !write_value(root, "scaling_governor", "schedutil")) return false;
    if (!read_value(root, "scaling_governor", current, sizeof(current)) || strcmp(current, "schedutil") != 0) {
        (void)write_value(root, "scaling_governor", "performance");
        return false;
    }
    fprintf(stderr, "CPU policy: menus use schedutil; emulator startup requests performance\n");
    return true;
}

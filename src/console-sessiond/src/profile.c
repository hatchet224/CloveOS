#include "sessiond/profile.h"
#include "sessiond/json.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool text(const struct cs_json *j, const char *key, char *out, size_t size)
{
    const char *value = cs_json_string(j, key);
    if (value == NULL || strlen(value) >= size) {
        return false;
    }
    strcpy(out, value);
    return true;
}
bool cs_profile_load(const char *path, struct cs_profile *p)
{
    static const char *const keys[] = {"game",
                                       "content",
                                       "core",
                                       "config",
                                       "bios",
                                       "retroarch",
                                       "disc",
                                       "policy",
                                       "card_path",
                                       "card_uuid",
                                       "bridge",
                                       "input_name",
                                       "input_path",
                                       "key_code",
                                       "bridge_uid",
                                       "max_state_bytes",
                                       "qualified_fat_syncfs",
                                       "attach_running"};
    char data[CS_JSON_MAX + 1U];
    size_t used = 0U;
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct cs_json j;
    bool ok = true;
    uint64_t code, uid;
    memset(p, 0, sizeof(*p));
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_uid != geteuid() && st.st_uid != 0U) || (st.st_mode & 0022U) != 0U ||
        st.st_size < 0 || st.st_size > (off_t)CS_JSON_MAX) {
        ok = false;
    }
    while (ok && used < sizeof(data)) {
        ssize_t n = read(fd, data + used, sizeof(data) - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok || used > CS_JSON_MAX || !cs_json_parse(&j, data, used) ||
        !cs_json_keys(&j, keys, sizeof(keys) / sizeof(keys[0]))) {
        return false;
    }
    if (!text(&j, "game", p->identity.game, 65U) ||
        !text(&j, "content", p->identity.content, 65U) ||
        !text(&j, "core", p->identity.core, 65U) || !text(&j, "config", p->identity.config, 65U) ||
        !text(&j, "bios", p->identity.bios, 65U) ||
        !text(&j, "retroarch", p->identity.retroarch, 65U) ||
        !text(&j, "disc", p->identity.disc, 65U) || !text(&j, "policy", p->identity.policy, 65U) ||
        !cs_identity_valid(&p->identity)) {
        return false;
    }
    if (!text(&j, "card_path", p->card_path, sizeof(p->card_path)) || p->card_path[0] != '/' ||
        !text(&j, "card_uuid", p->card_uuid, sizeof(p->card_uuid)) ||
        !text(&j, "bridge", p->bridge, sizeof(p->bridge)) || p->bridge[0] != '/' ||
        !text(&j, "input_name", p->input_name, sizeof(p->input_name)) || p->input_name[0] == '\0' ||
        !text(&j, "input_path", p->input_path, sizeof(p->input_path))) {
        return false;
    }
    if (!cs_json_u64(&j, "key_code", &code) || code > KEY_MAX ||
        !cs_json_u64(&j, "bridge_uid", &uid) || (uint64_t)(uid_t)uid != uid ||
        !cs_json_u64(&j, "max_state_bytes", &p->max_state_bytes) || p->max_state_bytes == 0U ||
        p->max_state_bytes > 128U * 1024U * 1024U ||
        !cs_json_bool(&j, "qualified_fat_syncfs", &p->qualified_fat_syncfs) ||
        !cs_json_bool(&j, "attach_running", &p->attach_running)) {
        return false;
    }
    p->key_code = (unsigned int)code;
    p->bridge_uid = (uid_t)uid;
    return strcmp(p->input_path, "auto") == 0 || p->input_path[0] == '/';
}

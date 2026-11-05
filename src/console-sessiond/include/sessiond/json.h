#ifndef SESSIOND_JSON_H
#define SESSIOND_JSON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CS_JSON_MAX 4096U
#define CS_JSON_FIELDS 32U
enum cs_json_type { CS_JSON_STRING, CS_JSON_NUMBER, CS_JSON_BOOL };
struct cs_json_field {
    char key[40];
    char value[512];
    enum cs_json_type type;
};
struct cs_json {
    struct cs_json_field fields[CS_JSON_FIELDS];
    size_t count;
};
/* Protocol/record subset: flat objects, unescaped printable ASCII strings,
 * unsigned decimal integers, booleans. Duplicate/unknown keys are rejected. */
bool cs_json_parse(struct cs_json *json, const char *data, size_t size);
const char *cs_json_string(const struct cs_json *json, const char *key);
bool cs_json_u64(const struct cs_json *json, const char *key, uint64_t *value);
bool cs_json_bool(const struct cs_json *json, const char *key, bool *value);
bool cs_json_keys(const struct cs_json *json, const char *const *keys, size_t count);
bool cs_decimal(const char *text, uint64_t *value);
#endif

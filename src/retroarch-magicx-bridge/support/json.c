#include "sessiond/json.h"
#include <string.h>
bool cs_decimal(const char *text, uint64_t *value)
{
    uint64_t result = 0U;
    size_t i;
    if (text == NULL || text[0] == '\0' || (text[0] == '0' && text[1] != '\0')) {
        return false;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        const unsigned char ch = (unsigned char)text[i];
        uint64_t digit;
        if (ch < '0' || ch > '9') {
            return false;
        }
        digit = (uint64_t)(ch - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}
static void whitespace(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t')) {
        ++*p;
    }
}
static bool string(const char **p, const char *end, char *out, size_t capacity)
{
    size_t n = 0U;
    if (*p == end || *(*p)++ != '"') {
        return false;
    }
    while (*p < end && **p != '"') {
        const unsigned char ch = (unsigned char)**p;
        if (ch < 32U || ch > 126U || ch == '\\' || n + 1U >= capacity) {
            return false;
        }
        out[n++] = *(*p)++;
    }
    if (*p == end) {
        return false;
    }
    ++*p;
    out[n] = '\0';
    return true;
}
static const struct cs_json_field *lookup(const struct cs_json *json, const char *key)
{
    size_t i;
    for (i = 0U; i < json->count; ++i) {
        if (strcmp(json->fields[i].key, key) == 0) {
            return &json->fields[i];
        }
    }
    return NULL;
}
bool cs_json_parse(struct cs_json *json, const char *data, size_t size)
{
    const char *p = data, *end = data + size;
    memset(json, 0, sizeof(*json));
    if (size == 0U || size > CS_JSON_MAX || memchr(data, '\0', size) != NULL) {
        return false;
    }
    whitespace(&p, end);
    if (p == end || *p++ != '{') {
        return false;
    }
    whitespace(&p, end);
    if (p < end && *p == '}') {
        ++p;
        whitespace(&p, end);
        return p == end;
    }
    while (p < end) {
        struct cs_json_field field;
        size_t n = 0U;
        memset(&field, 0, sizeof(field));
        if (json->count == CS_JSON_FIELDS || !string(&p, end, field.key, sizeof(field.key)) ||
            lookup(json, field.key) != NULL) {
            return false;
        }
        whitespace(&p, end);
        if (p == end || *p++ != ':') {
            return false;
        }
        whitespace(&p, end);
        if (p == end) {
            return false;
        }
        if (*p == '"') {
            field.type = CS_JSON_STRING;
            if (!string(&p, end, field.value, sizeof(field.value))) {
                return false;
            }
        } else {
            const char *start = p;
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' &&
                   *p != '\n') {
                ++p;
            }
            n = (size_t)(p - start);
            if (n == 0U || n >= sizeof(field.value)) {
                return false;
            }
            memcpy(field.value, start, n);
            field.value[n] = '\0';
            if (strcmp(field.value, "true") == 0 || strcmp(field.value, "false") == 0) {
                field.type = CS_JSON_BOOL;
            } else {
                uint64_t ignored;
                field.type = CS_JSON_NUMBER;
                if (!cs_decimal(field.value, &ignored)) {
                    return false;
                }
            }
        }
        json->fields[json->count++] = field;
        whitespace(&p, end);
        if (p == end) {
            return false;
        }
        if (*p == '}') {
            ++p;
            whitespace(&p, end);
            return p == end;
        }
        if (*p++ != ',') {
            return false;
        }
        whitespace(&p, end);
    }
    return false;
}
const char *cs_json_string(const struct cs_json *json, const char *key)
{
    const struct cs_json_field *field = lookup(json, key);
    return field != NULL && field->type == CS_JSON_STRING ? field->value : NULL;
}
bool cs_json_u64(const struct cs_json *json, const char *key, uint64_t *value)
{
    const struct cs_json_field *field = lookup(json, key);
    return field != NULL && (field->type == CS_JSON_STRING || field->type == CS_JSON_NUMBER) &&
           cs_decimal(field->value, value);
}
bool cs_json_bool(const struct cs_json *json, const char *key, bool *value)
{
    const struct cs_json_field *field = lookup(json, key);
    if (field == NULL || field->type != CS_JSON_BOOL) {
        return false;
    }
    *value = strcmp(field->value, "true") == 0;
    return true;
}
bool cs_json_keys(const struct cs_json *json, const char *const *keys, size_t count)
{
    size_t i;
    if (json->count != count) {
        return false;
    }
    for (i = 0U; i < count; ++i) {
        if (lookup(json, keys[i]) == NULL) {
            return false;
        }
    }
    return true;
}

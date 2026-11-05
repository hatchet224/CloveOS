#include "sessiond/document.h"
#include <limits.h>
#include <string.h>

bool cs_utf8_append(char *out, size_t capacity, size_t *used, uint32_t c) {
    unsigned char bytes[4];
    size_t n;
    if (c == 0U || c > 0x10ffffU || (c >= 0xd800U && c <= 0xdfffU))
        return false;
    if (c < 0x80U) {
        bytes[0] = (unsigned char)c;
        n = 1U;
    } else if (c < 0x800U) {
        bytes[0] = (unsigned char)(0xc0U | (c >> 6U));
        bytes[1] = (unsigned char)(0x80U | (c & 63U));
        n = 2U;
    } else if (c < 0x10000U) {
        bytes[0] = (unsigned char)(0xe0U | (c >> 12U));
        bytes[1] = (unsigned char)(0x80U | ((c >> 6U) & 63U));
        bytes[2] = (unsigned char)(0x80U | (c & 63U));
        n = 3U;
    } else {
        bytes[0] = (unsigned char)(0xf0U | (c >> 18U));
        bytes[1] = (unsigned char)(0x80U | ((c >> 12U) & 63U));
        bytes[2] = (unsigned char)(0x80U | ((c >> 6U) & 63U));
        bytes[3] = (unsigned char)(0x80U | (c & 63U));
        n = 4U;
    }
    if (*used >= capacity || n >= capacity - *used)
        return false;
    memcpy(out + *used, bytes, n);
    *used += n;
    out[*used] = '\0';
    return true;
}
static bool scalar(const char **p, const char *end, uint32_t *out) {
    unsigned char c;
    uint32_t v, minimum;
    unsigned int n, i;
    if (*p == end)
        return false;
    c = (unsigned char)*(*p)++;
    if (c < 0x80U) {
        *out = c;
        return c != 0U;
    }
    if (c >= 0xc2U && c <= 0xdfU) {
        v = c & 31U;
        n = 1U;
        minimum = 0x80U;
    } else if (c >= 0xe0U && c <= 0xefU) {
        v = c & 15U;
        n = 2U;
        minimum = 0x800U;
    } else if (c >= 0xf0U && c <= 0xf4U) {
        v = c & 7U;
        n = 3U;
        minimum = 0x10000U;
    } else
        return false;
    for (i = 0U; i < n; ++i) {
        if (*p == end || ((unsigned char)**p & 0xc0U) != 0x80U)
            return false;
        v = (v << 6U) | ((unsigned char)*(*p)++ & 63U);
    }
    if (v < minimum || v > 0x10ffffU || (v >= 0xd800U && v <= 0xdfffU))
        return false;
    *out = v;
    return true;
}
static bool hex4(const char **p, const char *end, uint32_t *out) {
    unsigned int i;
    uint32_t v = 0U;
    for (i = 0U; i < 4U; ++i) {
        unsigned char c;
        uint32_t digit;
        if (*p == end)
            return false;
        c = (unsigned char)*(*p)++;
        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a') + 10U;
        else if (c >= 'A' && c <= 'F')
            digit = (uint32_t)(c - 'A') + 10U;
        else
            return false;
        v = (v << 4U) | digit;
    }
    *out = v;
    return true;
}
bool cs_utf8_valid(const char *text, size_t bytes) {
    const char *p, *end;
    if (text == NULL || bytes > CS_DOC_BYTES)
        return false;
    p = text;
    end = text + bytes;
    while (p < end) {
        uint32_t cp;
        if (!scalar(&p, end, &cp))
            return false;
    }
    return true;
}
static bool character(const char **p, const char *end, uint32_t *out) {
    uint32_t c;
    if (*p == end)
        return false;
    if (**p != '\\')
        return scalar(p, end, out) && *out >= 32U;
    ++*p;
    if (*p == end)
        return false;
    switch (*(*p)++) {
    case '"':
        *out = '"';
        return true;
    case '\\':
        *out = '\\';
        return true;
    case '/':
        *out = '/';
        return true;
    case 'b':
        *out = 8U;
        return true;
    case 'f':
        *out = 12U;
        return true;
    case 'n':
        *out = 10U;
        return true;
    case 'r':
        *out = 13U;
        return true;
    case 't':
        *out = 9U;
        return true;
    case 'u':
        if (!hex4(p, end, &c))
            return false;
        if (c >= 0xd800U && c <= 0xdbffU) {
            uint32_t low;
            if (end - *p < 2 || (*p)[0] != '\\' || (*p)[1] != 'u')
                return false;
            *p += 2;
            if (!hex4(p, end, &low) || low < 0xdc00U || low > 0xdfffU)
                return false;
            c = 0x10000U + ((c - 0xd800U) << 10U) + low - 0xdc00U;
        }
        if (c == 0U || (c >= 0xdc00U && c <= 0xdfffU))
            return false;
        *out = c;
        return true;
    default:
        return false;
    }
}
bool cs_doc_text(const struct cs_document *d, size_t t, char *out, size_t capacity) {
    const char *p, *end;
    size_t used = 0U;
    if (t >= d->count || d->tokens[t].type != CS_DOC_STRING || capacity == 0U)
        return false;
    p = d->text + d->tokens[t].start + 1U;
    end = d->text + d->tokens[t].end - 1U;
    out[0] = '\0';
    while (p < end) {
        uint32_t c;
        if (!character(&p, end, &c) || !cs_utf8_append(out, capacity, &used, c))
            return false;
    }
    return p == end;
}
static void space(const struct cs_document *d, size_t *p) {
    while (*p < d->size && strchr(" \r\n\t", d->text[*p]) != NULL)
        ++*p;
}
static bool equal_keys(const struct cs_document *d, size_t a, size_t b) {
    const char *p = d->text + d->tokens[a].start + 1U;
    const char *q = d->text + d->tokens[b].start + 1U;
    const char *pe = d->text + d->tokens[a].end - 1U;
    const char *qe = d->text + d->tokens[b].end - 1U;
    while (p < pe && q < qe) {
        uint32_t x, y;
        if (!character(&p, pe, &x) || !character(&q, qe, &y) || x != y)
            return false;
    }
    return p == pe && q == qe;
}
static bool value(struct cs_document *d, size_t *p, unsigned int depth) {
    size_t t, first;
    char c;
    space(d, p);
    if (*p == d->size || depth > 16U || d->count == CS_DOC_TOKENS)
        return false;
    t = d->count++;
    d->tokens[t].start = *p;
    c = d->text[(*p)++];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        size_t members = 0U;
        d->tokens[t].type = c == '{' ? CS_DOC_OBJECT : CS_DOC_ARRAY;
        space(d, p);
        first = d->count;
        if (*p < d->size && d->text[*p] != close) {
            for (;;) {
                if (c == '{') {
                    size_t key = d->count, k;
                    char key_text[128];
                    if (++members > 128U)
                        return false;
                    if (!value(d, p, depth + 1U) || d->tokens[key].type != CS_DOC_STRING)
                        return false;
                    if (!cs_doc_text(d, key, key_text, sizeof(key_text)))
                        return false;
                    for (k = first; k < key; k = d->tokens[k + 1U].next)
                        if (equal_keys(d, k, key))
                            return false;
                    space(d, p);
                    if (*p == d->size || d->text[(*p)++] != ':')
                        return false;
                }
                if (!value(d, p, depth + 1U))
                    return false;
                space(d, p);
                if (*p == d->size)
                    return false;
                if (d->text[*p] == close)
                    break;
                if (d->text[(*p)++] != ',')
                    return false;
                space(d, p);
            }
        }
        if (*p == d->size || d->text[(*p)++] != close)
            return false;
    } else if (c == '"') {
        const char *q = d->text + *p, *end = d->text + d->size;
        d->tokens[t].type = CS_DOC_STRING;
        while (q < end && *q != '"') {
            uint32_t cp;
            if (!character(&q, end, &cp))
                return false;
        }
        if (q == end)
            return false;
        *p = (size_t)(q - d->text) + 1U;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        size_t begin;
        d->tokens[t].type = CS_DOC_NUMBER;
        *p = d->tokens[t].start;
        if (d->text[*p] == '-')
            ++*p;
        if (*p == d->size)
            return false;
        if (d->text[*p] == '0')
            ++*p;
        else {
            begin = *p;
            while (*p < d->size && d->text[*p] >= '0' && d->text[*p] <= '9')
                ++*p;
            if (*p == begin)
                return false;
        }
        if (*p < d->size && d->text[*p] == '.') {
            begin = ++*p;
            while (*p < d->size && d->text[*p] >= '0' && d->text[*p] <= '9')
                ++*p;
            if (*p == begin)
                return false;
        }
        if (*p < d->size && (d->text[*p] == 'e' || d->text[*p] == 'E')) {
            ++*p;
            if (*p < d->size && (d->text[*p] == '+' || d->text[*p] == '-'))
                ++*p;
            begin = *p;
            while (*p < d->size && d->text[*p] >= '0' && d->text[*p] <= '9')
                ++*p;
            if (*p == begin)
                return false;
        }
    } else {
        const char *literal;
        size_t length;
        if (c == 't') {
            literal = "true";
            d->tokens[t].type = CS_DOC_TRUE;
        } else if (c == 'f') {
            literal = "false";
            d->tokens[t].type = CS_DOC_FALSE;
        } else if (c == 'n') {
            literal = "null";
            d->tokens[t].type = CS_DOC_NULL;
        } else
            return false;
        length = strlen(literal);
        *p = d->tokens[t].start;
        if (length > d->size - *p || memcmp(d->text + *p, literal, length) != 0)
            return false;
        *p += length;
    }
    d->tokens[t].end = *p;
    d->tokens[t].next = d->count;
    return true;
}
bool cs_doc_parse(struct cs_document *d, const char *text, size_t size) {
    size_t p = 0U;
    if (d == NULL || text == NULL || size == 0U || size > CS_DOC_BYTES || memchr(text, 0, size))
        return false;
    d->text = text;
    d->size = size;
    d->count = 0U;
    if (!value(d, &p, 0U)) {
        d->count = 0U;
        return false;
    }
    space(d, &p);
    if (p != size) {
        d->count = 0U;
        return false;
    }
    return true;
}
size_t cs_doc_get(const struct cs_document *d, size_t object, const char *key) {
    size_t t;
    char name[128];
    if (object >= d->count || d->tokens[object].type != CS_DOC_OBJECT)
        return SIZE_MAX;
    for (t = object + 1U; t < d->tokens[object].next; t = d->tokens[t + 1U].next)
        if (cs_doc_text(d, t, name, sizeof(name)) && strcmp(name, key) == 0)
            return t + 1U;
    return SIZE_MAX;
}
bool cs_doc_int(const struct cs_document *d, size_t t, int *out) {
    size_t p;
    uint64_t v = 0U, limit;
    bool negative;
    if (t >= d->count || d->tokens[t].type != CS_DOC_NUMBER)
        return false;
    p = d->tokens[t].start;
    negative = d->text[p] == '-';
    if (negative)
        ++p;
    limit = negative ? (uint64_t)INT_MAX + 1U : (uint64_t)INT_MAX;
    for (; p < d->tokens[t].end; ++p) {
        unsigned char c = (unsigned char)d->text[p];
        if (c < '0' || c > '9' || v > (limit - (uint64_t)(c - '0')) / 10U)
            return false;
        v = v * 10U + (uint64_t)(c - '0');
    }
    *out = negative ? (v == (uint64_t)INT_MAX + 1U ? INT_MIN : -(int)v) : (int)v;
    return true;
}
bool cs_doc_bool(const struct cs_document *d, size_t t, bool *out) {
    if (t >= d->count || (d->tokens[t].type != CS_DOC_TRUE && d->tokens[t].type != CS_DOC_FALSE))
        return false;
    *out = d->tokens[t].type == CS_DOC_TRUE;
    return true;
}

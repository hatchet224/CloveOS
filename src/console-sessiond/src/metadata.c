#include "sessiond/library.h"
#include "sessiond/document.h"
#include <stdlib.h>
#include <string.h>

bool cs_card_resolve(const char *base, const char *path, char out[512]) {
    char joined[1536];
    size_t used = 0U, i = 0U;
    const char *prefix = "/mnt/SDCARD/";
    if (out == NULL || base == NULL || path == NULL || strlen(base) > 511U || strlen(path) > 511U)
        return false;
    if (path[0] == '/') {
        if (strncmp(path, prefix, strlen(prefix)) != 0)
            return false;
        base = "";
        path += strlen(prefix);
    }
    if (strlen(base) + strlen(path) + 2U > sizeof(joined))
        return false;
    strcpy(joined, base);
    strcat(joined, "/");
    strcat(joined, path);
    out[0] = '\0';
    while (joined[i] != '\0') {
        size_t begin, n, j;
        while (joined[i] == '/')
            ++i;
        begin = i;
        while (joined[i] != '/' && joined[i] != '\0')
            ++i;
        n = i - begin;
        if (n == 0U || (n == 1U && joined[begin] == '.'))
            continue;
        if (n == 2U && joined[begin] == '.' && joined[begin + 1U] == '.') {
            if (used == 0U)
                return false;
            while (used > 0U && out[used - 1U] != '/')
                --used;
            if (used > 0U)
                --used;
            out[used] = '\0';
            continue;
        }
        if (n > 255U || used + n + (used > 0U ? 1U : 0U) >= 512U)
            return false;
        for (j = begin; j < i; ++j)
            if ((unsigned char)joined[j] < 32U || joined[j] == '\\' || joined[j] == ':')
                return false;
        if (used > 0U)
            out[used++] = '/';
        memcpy(out + used, joined + begin, n);
        used += n;
        out[used] = '\0';
    }
    return used != 0U;
}
static bool json_field(const struct cs_document *d, size_t object, const char *key,
                       const char *alternate, char *out, size_t cap, bool required) {
    size_t t = cs_doc_get(d, object, key);
    if (t == SIZE_MAX && alternate != NULL)
        t = cs_doc_get(d, object, alternate);
    if (t == SIZE_MAX) {
        out[0] = '\0';
        return !required;
    }
    if (!cs_doc_text(d, t, out, cap) || (required && out[0] == '\0'))
        return false;
    {
        size_t i;
        for (i = 0U; out[i] != '\0'; ++i)
            if ((unsigned char)out[i] < 32U || (unsigned char)out[i] == 127U)
                return false;
    }
    return true;
}
static bool json_game(const struct cs_document *d, size_t node, struct cs_metadata_list *list) {
    struct cs_metadata *game;
    if (list->count == CS_METADATA_MAX || d->tokens[node].type != CS_DOC_OBJECT)
        return false;
    game = &list->games[list->count];
    if (!json_field(d, node, "path", "rompath", game->path, sizeof(game->path), true) ||
        !json_field(d, node, "name", "label", game->name, sizeof(game->name), false) ||
        !json_field(d, node, "image", "imgpath", game->image, sizeof(game->image), false))
        return false;
    ++list->count;
    return true;
}
bool cs_metadata_parse_json(const char *text, size_t bytes, struct cs_metadata_list *out) {
    struct cs_document *d = calloc(1U, sizeof(*d));
    struct cs_metadata_list *list = calloc(1U, sizeof(*list));
    size_t array, t;
    bool ok = false;
    if (d == NULL || list == NULL || out == NULL || text == NULL || bytes == 0U ||
        bytes > CS_DOC_BYTES)
        goto done;
    if (cs_doc_parse(d, text, bytes)) {
        array = d->tokens[0].type == CS_DOC_ARRAY ? 0U : cs_doc_get(d, 0U, "games");
        if (array == SIZE_MAX) {
            if (!json_game(d, 0U, list))
                goto done;
        } else {
            if (array >= d->count || d->tokens[array].type != CS_DOC_ARRAY)
                goto done;
            for (t = array + 1U; t < d->tokens[array].next; t = d->tokens[t].next)
                if (!json_game(d, t, list))
                    goto done;
        }
    } else {
        /* Onion recent/favorite caches use one JSON object per line. Import
         * names/art only; their order and launch scripts are never authority. */
        size_t start = 0U;
        while (start < bytes) {
            const char *newline = memchr(text + start, '\n', bytes - start);
            size_t end = newline == NULL ? bytes : (size_t)(newline - text), begin = start;
            while (begin < end && strchr(" \t\r", text[begin]) != NULL)
                ++begin;
            if (begin < end &&
                (!cs_doc_parse(d, text + begin, end - begin) || !json_game(d, 0U, list)))
                goto done;
            start = newline == NULL ? bytes : end + 1U;
        }
        if (list->count == 0U)
            goto done;
    }
    *out = *list;
    ok = true;
done:
    free(d);
    free(list);
    return ok;
}
static bool xml_text(const char *p, const char *end, char *out, size_t capacity, bool entities) {
    size_t used = 0U;
    out[0] = '\0';
    while (p < end) {
        if (*p == '&' && entities) {
            const char *semi = memchr(p, ';', (size_t)(end - p));
            uint32_t c = 0U;
            size_t n;
            if (semi == NULL || (n = (size_t)(semi - p)) > 12U)
                return false;
            if (n == 4U && memcmp(p, "&amp", 4U) == 0)
                c = '&';
            else if (n == 3U && memcmp(p, "&lt", 3U) == 0)
                c = '<';
            else if (n == 3U && memcmp(p, "&gt", 3U) == 0)
                c = '>';
            else if (n == 5U && memcmp(p, "&quot", 5U) == 0)
                c = '"';
            else if (n == 5U && memcmp(p, "&apos", 5U) == 0)
                c = '\'';
            else if (n >= 3U && p[1] == '#') {
                const char *q = p + 2;
                unsigned int base = 10U;
                if (*q == 'x') {
                    ++q;
                    base = 16U;
                }
                if (q == semi)
                    return false;
                for (; q < semi; ++q) {
                    unsigned int digit;
                    if (*q >= '0' && *q <= '9')
                        digit = (unsigned int)(*q - '0');
                    else if (*q >= 'a' && *q <= 'f')
                        digit = (unsigned int)(*q - 'a') + 10U;
                    else if (*q >= 'A' && *q <= 'F')
                        digit = (unsigned int)(*q - 'A') + 10U;
                    else
                        return false;
                    if (digit >= base || c > (0x10ffffU - digit) / base)
                        return false;
                    c = c * base + digit;
                }
            } else
                return false;
            if (c < 32U || !cs_utf8_append(out, capacity, &used, c))
                return false;
            p = semi + 1;
        } else {
            unsigned char c = (unsigned char)*p++;
            if (c == 0U || c < 32U || used + 1U >= capacity)
                return false;
            out[used++] = (char)c;
            out[used] = '\0';
        }
    }
    return true;
}
/* Purposefully small XML subset, with balanced tags and a single gameList root.
 * DTDs, entities, processing instructions other than the initial XML declaration,
 * and nested markup in the three consumed leaf fields are rejected. */
bool cs_metadata_parse_xml(const char *text, size_t bytes, struct cs_metadata_list *out) {
    struct cs_metadata_list *list = calloc(1U, sizeof(*list));
    char tags[16][48];
    size_t depth = 0U;
    const char *p, *end;
    bool root = false, closed = false, ok = false;
    unsigned int fields = 0U;
    if (list == NULL || out == NULL || text == NULL || bytes == 0U || bytes > CS_DOC_BYTES ||
        !cs_utf8_valid(text, bytes))
        goto done;
    p = text;
    end = text + bytes;
    if (bytes >= 3U && memcmp(text, "\xef\xbb\xbf", 3U) == 0)
        p += 3;
    {
        const char *q;
        for (q = p; q < end; ++q)
            if (((unsigned char)*q < 32U && strchr("\r\n\t", *q) == NULL) ||
                (unsigned char)*q == 127U)
                goto done;
    }
    while (p < end) {
        if (*p != '<') {
            const char *q = memchr(p, '<', (size_t)(end - p));
            if (q == NULL)
                q = end;
            if (depth == 3U && strcmp(tags[1], "game") == 0 && list->count < CS_METADATA_MAX) {
                struct cs_metadata *g = &list->games[list->count];
                char *dest = NULL;
                size_t cap = 0U;
                unsigned int bit = 0U;
                if (strcmp(tags[2], "path") == 0) {
                    dest = g->path;
                    cap = sizeof(g->path);
                    bit = 1U;
                } else if (strcmp(tags[2], "name") == 0) {
                    dest = g->name;
                    cap = sizeof(g->name);
                    bit = 2U;
                } else if (strcmp(tags[2], "image") == 0) {
                    dest = g->image;
                    cap = sizeof(g->image);
                    bit = 4U;
                }
                if (dest != NULL) {
                    if ((fields & bit) != 0U || !xml_text(p, q, dest, cap, true))
                        goto done;
                    fields |= bit;
                }
            } else if (depth < 2U) {
                const char *q2;
                for (q2 = p; q2 < q; ++q2)
                    if (strchr(" \r\n\t", *q2) == NULL)
                        goto done;
            }
            p = q;
            continue;
        }
        if (end - p >= 4 && memcmp(p, "<!--", 4U) == 0) {
            const char *q = p + 4;
            while (end - q >= 3 && memcmp(q, "-->", 3U) != 0)
                ++q;
            if (end - q < 3)
                goto done;
            p = q + 3;
            continue;
        }
        if (end - p >= 9 && memcmp(p, "<![CDATA[", 9U) == 0) {
            const char *q = p + 9;
            char *dest = NULL;
            size_t cap = 0U;
            unsigned int bit = 0U;
            if (depth != 3U || strcmp(tags[1], "game") != 0 || list->count == CS_METADATA_MAX)
                goto done;
            while (end - q >= 3 && memcmp(q, "]]>", 3U) != 0)
                ++q;
            if (end - q < 3)
                goto done;
            if (strcmp(tags[2], "path") == 0) {
                dest = list->games[list->count].path;
                cap = 512U;
                bit = 1U;
            } else if (strcmp(tags[2], "name") == 0) {
                dest = list->games[list->count].name;
                cap = 97U;
                bit = 2U;
            } else if (strcmp(tags[2], "image") == 0) {
                dest = list->games[list->count].image;
                cap = 512U;
                bit = 4U;
            }
            if (dest != NULL) {
                if ((fields & bit) != 0U || !xml_text(p + 9, q, dest, cap, false))
                    goto done;
                fields |= bit;
            }
            p = q + 3;
            continue;
        }
        if (end - p >= 5 && memcmp(p, "<?xml", 5U) == 0 && !root && (p == text || p == text + 3)) {
            const char *q = p + 5;
            while (end - q >= 2 && memcmp(q, "?>", 2U) != 0)
                ++q;
            if (end - q < 2)
                goto done;
            p = q + 2;
            continue;
        }
        {
            char name[48];
            size_t n = 0U;
            bool closing, self = false;
            char quote = '\0';
            ++p;
            closing = p < end && *p == '/';
            if (closing)
                ++p;
            while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
                if (n + 1U >= sizeof(name))
                    goto done;
                name[n++] = *p++;
            }
            name[n] = '\0';
            if (n == 0U)
                goto done;
            if (closing) {
                while (p < end && strchr(" \r\n\t", *p))
                    ++p;
                if (p == end || *p++ != '>' || depth == 0U || strcmp(tags[depth - 1U], name) != 0)
                    goto done;
                if (depth == 2U && strcmp(name, "game") == 0) {
                    if (list->count == CS_METADATA_MAX || (fields & 1U) == 0U ||
                        list->games[list->count].path[0] == '\0')
                        goto done;
                    ++list->count;
                }
                --depth;
                if (depth == 0U)
                    closed = true;
            } else {
                if (p < end && *p != '/' && *p != '>' && strchr(" \r\n\t", *p) == NULL)
                    goto done;
                while (p < end) {
                    char c = *p++;
                    if (quote != '\0') {
                        if (c == quote)
                            quote = '\0';
                    } else if (c == '\'' || c == '"')
                        quote = c;
                    else if (c == '<')
                        goto done;
                    else if (c == '>')
                        break;
                }
                if (p == end && end[-1] != '>')
                    goto done;
                if (quote != '\0' || depth == 16U || closed)
                    goto done;
                self = p - text >= 2 && p[-2] == '/';
                if (depth == 0U) {
                    if (root || strcmp(name, "gameList") != 0)
                        goto done;
                    root = true;
                }
                if (depth == 1U && strcmp(name, "game") == 0) {
                    if (list->count == CS_METADATA_MAX || self)
                        goto done;
                    fields = 0U;
                }
                if (depth == 3U && (strcmp(tags[2], "path") == 0 || strcmp(tags[2], "name") == 0 ||
                                    strcmp(tags[2], "image") == 0))
                    goto done;
                if (!self) {
                    strcpy(tags[depth], name);
                    ++depth;
                } else if (depth == 0U)
                    closed = true;
            }
        }
    }
    if (!root || !closed || depth != 0U)
        goto done;
    *out = *list;
    ok = true;
done:
    free(list);
    return ok;
}

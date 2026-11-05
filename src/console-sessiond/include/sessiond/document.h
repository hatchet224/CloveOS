#ifndef SESSIOND_DOCUMENT_H
#define SESSIOND_DOCUMENT_H
/* Bounded nested JSON for UI/library data. The strict bridge wire parser stays
 * separate: accepting a theme must never relax the checkpoint protocol. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CS_DOC_TOKENS 8192U
#define CS_DOC_BYTES (1024U * 1024U)
enum cs_doc_type {
    CS_DOC_OBJECT,
    CS_DOC_ARRAY,
    CS_DOC_STRING,
    CS_DOC_NUMBER,
    CS_DOC_TRUE,
    CS_DOC_FALSE,
    CS_DOC_NULL
};
struct cs_doc_token {
    size_t start, end, next;
    enum cs_doc_type type;
};
struct cs_document {
    const char *text;
    size_t size, count;
    struct cs_doc_token tokens[CS_DOC_TOKENS];
};
/* Caller owns text and document. No allocations; depth <= 16; duplicate keys,
 * malformed escapes, overlong UTF-8, NULs and trailing bytes are rejected. */
bool cs_doc_parse(struct cs_document *doc, const char *text, size_t size);
size_t cs_doc_get(const struct cs_document *doc, size_t object, const char *key);
bool cs_doc_text(const struct cs_document *doc, size_t token, char *out, size_t capacity);
bool cs_doc_int(const struct cs_document *doc, size_t token, int *out);
bool cs_doc_bool(const struct cs_document *doc, size_t token, bool *out);
/* Shared XML/JSON entity decoder primitive: append one valid Unicode scalar. */
bool cs_utf8_append(char *out, size_t capacity, size_t *used, uint32_t scalar);
bool cs_utf8_valid(const char *text, size_t bytes);
#endif

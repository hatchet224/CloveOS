#ifndef SUPERVISOR_API_DOCUMENT_H
#define SUPERVISOR_API_DOCUMENT_H
#include "sessiond/document.h"
/* External game records include large ROM/media lists. Keep their capacity
 * separate from the unchanged UI/session document type and wire protocol. */
#define SC_API_DOC_TOKENS 32768U
struct sc_api_document {
    const char *text;
    size_t size, count;
    struct cs_doc_token tokens[SC_API_DOC_TOKENS];
};
bool sc_api_doc_parse(struct sc_api_document *doc, const char *text, size_t size);
size_t sc_api_doc_get(const struct sc_api_document *doc, size_t object, const char *key);
bool sc_api_doc_text(const struct sc_api_document *doc, size_t token, char *out, size_t capacity);
bool sc_api_doc_int(const struct sc_api_document *doc, size_t token, int *out);
bool sc_api_doc_bool(const struct sc_api_document *doc, size_t token, bool *out);
bool sc_api_utf8_append(char *out, size_t capacity, size_t *used, uint32_t scalar);
bool sc_api_utf8_valid(const char *text, size_t bytes);
#endif

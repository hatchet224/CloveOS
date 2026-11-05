#include "supervisor/api_document.h"
/* Instantiate the same strict parser under a private namespace/capacity.
 * The normal sessiond/document.h ABI, 8192-token bound, duplicate-key checks,
 * UTF-8 checks, depth limit and 1 MiB byte limit remain unchanged. */
#undef CS_DOC_TOKENS
#define CS_DOC_TOKENS SC_API_DOC_TOKENS
#define cs_document sc_api_document
#define cs_doc_parse sc_api_doc_parse
#define cs_doc_get sc_api_doc_get
#define cs_doc_text sc_api_doc_text
#define cs_doc_int sc_api_doc_int
#define cs_doc_bool sc_api_doc_bool
#define cs_utf8_append sc_api_utf8_append
#define cs_utf8_valid sc_api_utf8_valid
#include "../../console-sessiond/src/document.c"

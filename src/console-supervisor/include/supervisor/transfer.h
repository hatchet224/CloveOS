#ifndef SUPERVISOR_TRANSFER_H
#define SUPERVISOR_TRANSFER_H
#include "sessiond/store.h"
#include <stddef.h>
struct sc_transfer {
    unsigned char *bytes;
    size_t size;
    long http_status;
    int transport_error; /* libcurl result code; never contains a URL or secret. */
};
struct sc_transfer_cancel {
    bool (*cancelled)(void *context);
    void *context;
};
/* Worker-only HTTPS, verified certificates/hostname, bounded bytes and time.
 * No cookies, proxy environment, credentials in logs, HTTP fallback or shell.
 * Free bytes with sc_transfer_close even when an operation fails. */
bool sc_transfer_get(const char *url, size_t limit, const struct sc_transfer_cancel *cancel,
                       struct sc_transfer *result);
void sc_transfer_close(struct sc_transfer *result);
/* Qualified mounted-card transaction. All relative components reject symlinks
 * and mount crossings. previous data survives network/validation failures;
 * success requires file fsync, atomic rename and directory/media barriers. */
bool sc_transfer_commit(const struct cs_store *store, const char *directory,
                          const char *name, const unsigned char *bytes, size_t size);
/* Explicit CA path exists for isolated TLS test fixtures. Production uses the
 * immutable /etc/console/ca-certificates.crt file and never disables verification. */
bool sc_transfer_get_ca(const char *url, const char *ca, size_t limit,
                          const struct sc_transfer_cancel *cancel, struct sc_transfer *result);
#endif

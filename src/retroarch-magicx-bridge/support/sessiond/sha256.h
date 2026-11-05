#ifndef SESSIOND_SHA256_H
#define SESSIOND_SHA256_H
#include <stddef.h>
#include <stdint.h>
struct cs_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
};
void cs_sha256_init(struct cs_sha256 *hash);
void cs_sha256_update(struct cs_sha256 *hash, const void *data, size_t size);
void cs_sha256_final(struct cs_sha256 *hash, char hex[65]);
void cs_sha256_hex(const void *data, size_t size, char hex[65]);
int cs_hex_valid(const char *text, size_t digits);
#endif

#include "sessiond/sha256.h"
#include <string.h>
#if defined(__aarch64__) && defined(__linux__)
#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif
static uint32_t rotate(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32U - n));
}
static const uint32_t round_constants[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
#if defined(__aarch64__) && defined(__linux__)
/* Called only after the kernel advertises SHA2 in HWCAP. Other ARM64 CPUs
 * retain the scalar path, including old kernels without crypto capabilities. */
__attribute__((target("sha2")))
static void transform_sha2(struct cs_sha256 *hash, const unsigned char *data, size_t blocks)
{
    uint32x4_t a = vld1q_u32(hash->h), b = vld1q_u32(hash->h + 4U);
    while (blocks-- != 0U) {
        uint32x4_t first = a, second = b, message[4];
        for (size_t i = 0U; i < 4U; ++i)
            message[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + i * 16U)));
        for (size_t i = 0U; i < 16U; ++i) {
            uint32x4_t previous = a;
            uint32x4_t wk = vaddq_u32(message[i % 4U], vld1q_u32(round_constants + i * 4U));
            a = vsha256hq_u32(a, b, wk);
            b = vsha256h2q_u32(b, previous, wk);
            if (i < 12U)
                message[i % 4U] = vsha256su1q_u32(
                    vsha256su0q_u32(message[i % 4U], message[(i + 1U) % 4U]),
                    message[(i + 2U) % 4U], message[(i + 3U) % 4U]);
        }
        a = vaddq_u32(a, first);
        b = vaddq_u32(b, second);
        data += 64U;
    }
    vst1q_u32(hash->h, a);
    vst1q_u32(hash->h + 4U, b);
}
#endif
static void transform_scalar(struct cs_sha256 *hash, const unsigned char *data)
{
    const uint32_t *k = round_constants;
    uint32_t w[64], a, b, c, d, e, f, g, h;
    size_t i;
    for (i = 0U; i < 16U; ++i) {
        const size_t j = i * 4U;
        w[i] = ((uint32_t)data[j] << 24U) | ((uint32_t)data[j + 1U] << 16U) |
               ((uint32_t)data[j + 2U] << 8U) | data[j + 3U];
    }
    for (; i < 64U; ++i) {
        const uint32_t s0 = rotate(w[i - 15U], 7U) ^ rotate(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
        const uint32_t s1 = rotate(w[i - 2U], 17U) ^ rotate(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    a = hash->h[0];
    b = hash->h[1];
    c = hash->h[2];
    d = hash->h[3];
    e = hash->h[4];
    f = hash->h[5];
    g = hash->h[6];
    h = hash->h[7];
    for (i = 0U; i < 64U; ++i) {
        const uint32_t t1 = h + (rotate(e, 6U) ^ rotate(e, 11U) ^ rotate(e, 25U)) +
                            ((e & f) ^ ((~e) & g)) + k[i] + w[i];
        const uint32_t t2 =
            (rotate(a, 2U) ^ rotate(a, 13U) ^ rotate(a, 22U)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    hash->h[0] += a;
    hash->h[1] += b;
    hash->h[2] += c;
    hash->h[3] += d;
    hash->h[4] += e;
    hash->h[5] += f;
    hash->h[6] += g;
    hash->h[7] += h;
}
static void transform(struct cs_sha256 *hash, const unsigned char *data, size_t blocks)
{
#if defined(__aarch64__) && defined(__linux__)
    if ((getauxval(AT_HWCAP) & HWCAP_SHA2) != 0U) {
        transform_sha2(hash, data, blocks);
        return;
    }
#else
    (void)round_constants;
#endif
    while (blocks-- != 0U) {
        transform_scalar(hash, data);
        data += 64U;
    }
}
void cs_sha256_init(struct cs_sha256 *hash)
{
    static const uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    memset(hash, 0, sizeof(*hash));
    memcpy(hash->h, initial, sizeof(initial));
}
void cs_sha256_update(struct cs_sha256 *hash, const void *data, size_t size)
{
    const unsigned char *p = data;
    hash->bytes += (uint64_t)size;
    while (size > 0U) {
        if (hash->used == 0U && size >= 64U) {
            size_t blocks = size / 64U;
            transform(hash, p, blocks);
            p += blocks * 64U;
            size -= blocks * 64U;
            continue;
        }
        size_t n = 64U - hash->used;
        if (n > size) {
            n = size;
        }
        memcpy(hash->block + hash->used, p, n);
        hash->used += n;
        p += n;
        size -= n;
        if (hash->used == 64U) {
            transform(hash, hash->block, 1U);
            hash->used = 0U;
        }
    }
}
void cs_sha256_final(struct cs_sha256 *hash, char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    const uint64_t bits = hash->bytes * 8U;
    unsigned char padding[128] = {0x80U};
    const size_t n = hash->used < 56U ? 56U - hash->used : 120U - hash->used;
    size_t i;
    cs_sha256_update(hash, padding, n);
    for (i = 0U; i < 8U; ++i) {
        padding[i] = (unsigned char)(bits >> (56U - (unsigned int)i * 8U));
    }
    cs_sha256_update(hash, padding, 8U);
    for (i = 0U; i < 32U; ++i) {
        const unsigned int shift = 24U - (unsigned int)(i % 4U) * 8U;
        const unsigned char b = (unsigned char)(hash->h[i / 4U] >> shift);
        hex[i * 2U] = digits[b >> 4U];
        hex[i * 2U + 1U] = digits[b & 15U];
    }
    hex[64] = '\0';
}
void cs_sha256_hex(const void *data, size_t size, char hex[65])
{
    struct cs_sha256 hash;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, data, size);
    cs_sha256_final(&hash, hex);
}
int cs_hex_valid(const char *text, size_t digits)
{
    size_t i;
    if (text == NULL || strlen(text) != digits) {
        return 0;
    }
    for (i = 0U; i < digits; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

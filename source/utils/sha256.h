#ifndef NINJAGO_SHA256_H
#define NINJAGO_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct NjSha256 {
    uint32_t state[8];
    uint64_t bytes;
    size_t used;
    uint8_t block[64];
} NjSha256;
void nj_sha256_init(NjSha256 *ctx);
void nj_sha256_update(NjSha256 *ctx, const void *data, size_t size);
void nj_sha256_final(NjSha256 *ctx, uint8_t digest[32]);
#endif

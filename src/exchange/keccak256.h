#ifndef TB_KECCAK256_H
#define TB_KECCAK256_H

/*
 * Keccak-256 hash (Ethereum variant).
 *
 * IMPORTANT: This is NOT the same as SHA3-256 (FIPS 202).
 * Ethereum uses the original Keccak submission which has a different
 * padding scheme (0x01 vs 0x06 for SHA3). OpenSSL's EVP_sha3_256
 * produces DIFFERENT output and cannot be used here.
 *
 * Reference: https://keccak.team/keccak_specs_summary.html
 */

#include <stdint.h>
#include <stddef.h>

#define KECCAK256_DIGEST_SIZE 32

/* Compute keccak256 hash of data */
void keccak256(const uint8_t *data, size_t len, uint8_t *out);

/* Incremental hashing */
typedef struct {
    uint64_t state[25];
    uint8_t  buf[136];  /* rate = 1088 bits = 136 bytes for keccak256 */
    size_t   buf_len;
} keccak256_ctx_t;

void keccak256_init(keccak256_ctx_t *ctx);
void keccak256_update(keccak256_ctx_t *ctx, const uint8_t *data, size_t len);
void keccak256_final(keccak256_ctx_t *ctx, uint8_t *out);

#endif /* TB_KECCAK256_H */

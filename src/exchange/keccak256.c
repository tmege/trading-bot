/*
 * Keccak-256 implementation (Ethereum variant, NOT SHA3-256).
 *
 * Based on the Keccak reference implementation.
 * Padding: 0x01 (original Keccak), NOT 0x06 (SHA3/FIPS 202).
 *
 * Rate = 1088 bits (136 bytes), Capacity = 512 bits, Output = 256 bits.
 */

#include "exchange/keccak256.h"
#include <string.h>

/* Keccak-f[1600] round constants */
static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

/* Rotation offsets */
static const int ROTC[24] = {
     1,  3,  6, 10, 15, 21, 28, 36, 45, 55,  2, 14,
    27, 41, 56,  8, 25, 43, 62, 18, 39, 61, 20, 44,
};

/* Pi permutation indices */
static const int PILN[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,  8, 21, 24,  4,
    15, 23, 19, 13, 12,  2, 20, 14, 22,  9,  6,  1,
};

static inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

/* Keccak-f[1600] permutation */
static void keccak_f(uint64_t state[25]) {
    for (int round = 0; round < 24; round++) {
        uint64_t bc[5];

        /* Theta */
        for (int i = 0; i < 5; i++)
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^
                    state[i + 15] ^ state[i + 20];

        for (int i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                state[j + i] ^= t;
        }

        /* Rho and Pi */
        uint64_t t = state[1];
        for (int i = 0; i < 24; i++) {
            int j = PILN[i];
            uint64_t tmp = state[j];
            state[j] = rotl64(t, ROTC[i]);
            t = tmp;
        }

        /* Chi */
        for (int j = 0; j < 25; j += 5) {
            uint64_t tmp[5];
            for (int i = 0; i < 5; i++)
                tmp[i] = state[j + i];
            for (int i = 0; i < 5; i++)
                state[j + i] = tmp[i] ^ ((~tmp[(i + 1) % 5]) & tmp[(i + 2) % 5]);
        }

        /* Iota */
        state[0] ^= RC[round];
    }
}

#define KECCAK256_RATE 136  /* (1600 - 2*256) / 8 */

void keccak256_init(keccak256_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void keccak256_update(keccak256_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;

    /* Fill buffer first */
    if (ctx->buf_len > 0) {
        size_t fill = KECCAK256_RATE - ctx->buf_len;
        if (len < fill) {
            memcpy(ctx->buf + ctx->buf_len, data, len);
            ctx->buf_len += len;
            return;
        }
        memcpy(ctx->buf + ctx->buf_len, data, fill);

        /* XOR buffer into state */
        for (size_t j = 0; j < KECCAK256_RATE / 8; j++) {
            uint64_t lane;
            memcpy(&lane, ctx->buf + j * 8, 8);
            ctx->state[j] ^= lane;
        }
        keccak_f(ctx->state);

        i = fill;
        ctx->buf_len = 0;
    }

    /* Process full blocks */
    while (i + KECCAK256_RATE <= len) {
        for (size_t j = 0; j < KECCAK256_RATE / 8; j++) {
            uint64_t lane;
            memcpy(&lane, data + i + j * 8, 8);
            ctx->state[j] ^= lane;
        }
        keccak_f(ctx->state);
        i += KECCAK256_RATE;
    }

    /* Buffer remaining */
    size_t remaining = len - i;
    if (remaining > 0) {
        memcpy(ctx->buf, data + i, remaining);
        ctx->buf_len = remaining;
    }
}

void keccak256_final(keccak256_ctx_t *ctx, uint8_t *out) {
    /* Pad: original Keccak uses 0x01, NOT SHA3's 0x06 */
    memset(ctx->buf + ctx->buf_len, 0, KECCAK256_RATE - ctx->buf_len);
    ctx->buf[ctx->buf_len] = 0x01;
    ctx->buf[KECCAK256_RATE - 1] |= 0x80;

    /* XOR padded block into state */
    for (size_t j = 0; j < KECCAK256_RATE / 8; j++) {
        uint64_t lane;
        memcpy(&lane, ctx->buf + j * 8, 8);
        ctx->state[j] ^= lane;
    }
    keccak_f(ctx->state);

    /* Extract output (first 256 bits = 32 bytes) */
    memcpy(out, ctx->state, KECCAK256_DIGEST_SIZE);
}

void keccak256(const uint8_t *data, size_t len, uint8_t *out) {
    keccak256_ctx_t ctx;
    keccak256_init(&ctx);
    keccak256_update(&ctx, data, len);
    keccak256_final(&ctx, out);
}

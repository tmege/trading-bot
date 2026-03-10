#ifndef TB_HL_SIGNING_H
#define TB_HL_SIGNING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* EIP-712 signature (r, s, v) */
typedef struct {
    uint8_t r[32];
    uint8_t s[32];
    uint8_t v;  /* 27 or 28 */
} hl_signature_t;

/* Opaque signer context */
typedef struct hl_signer hl_signer_t;

/* Create signer from hex-encoded private key (without 0x prefix) */
hl_signer_t *hl_signer_create(const char *private_key_hex);
void         hl_signer_destroy(hl_signer_t *signer);

/* Get wallet address (checksummed hex with 0x prefix) */
int hl_signer_get_address(const hl_signer_t *signer, char *out, size_t out_len);

/*
 * Sign an L1 action for Hyperliquid.
 *
 * Process:
 *   1. hash = keccak256(msgpack_data || nonce_le_8bytes [|| vault_addr_20bytes])
 *   2. Construct phantom Agent { source: "a"|"b", connectionId: hash }
 *   3. EIP-712 sign Agent with domain { "Exchange", "1", 1337, 0x0...0 }
 *
 * Parameters:
 *   msgpack_data/len: pre-serialized msgpack of the action
 *   nonce: timestamp in milliseconds
 *   vault_address: NULL if not using vault
 *   is_mainnet: true for mainnet ("a"), false for testnet ("b")
 */
int hl_sign_l1_action(
    const hl_signer_t *signer,
    const uint8_t *msgpack_data,
    size_t msgpack_len,
    uint64_t nonce,
    const char *vault_address,
    bool is_mainnet,
    hl_signature_t *out_sig
);

/* Format signature as hex string: "0x" + r(64) + s(64) + v(2) = 132 chars + null */
int hl_signature_to_hex(const hl_signature_t *sig, char *buf, size_t buf_len);

#endif /* TB_HL_SIGNING_H */

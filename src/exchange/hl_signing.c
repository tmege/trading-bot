/*
 * EIP-712 signing for Hyperliquid L1 actions.
 *
 * Domain: { name: "Exchange", version: "1", chainId: 1337, verifyingContract: 0x0...0 }
 * Primary type: Agent { source: string, connectionId: bytes32 }
 *
 * The phantom Agent is constructed from the action hash:
 *   connectionId = keccak256(msgpack(action) || nonce_le_8bytes)
 *   source = 0x61 ("a") for mainnet, 0x62 ("b") for testnet
 */

#include "exchange/hl_signing.h"
#include "exchange/keccak256.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>

/* Secure memory wipe — explicit_bzero is guaranteed by the platform ABI
 * to never be optimized away, unlike volatile pointer loops under LTO. */
static void secure_wipe(void *ptr, size_t len) {
    explicit_bzero(ptr, len);
}

struct hl_signer {
    secp256k1_context *secp_ctx;
    uint8_t            privkey[32];
    uint8_t            pubkey_uncompressed[65];
    uint8_t            address[20];
};

/* ── Hex helpers ────────────────────────────────────────────────────────────── */

static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    /* Skip 0x prefix */
    if (hex_len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        hex_len -= 2;
    }
    if (hex_len != out_len * 2) return -1;

    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

static void hex_encode(const uint8_t *data, size_t len, char *out, size_t out_len) {
    if (out_len < len * 2 + 1) { out[0] = '\0'; return; }
    for (size_t i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

/* ── Pre-computed EIP-712 constants ────────────────────────────────────────── */

/*
 * domainSeparator = keccak256(
 *   keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
 *   || keccak256("Exchange")
 *   || keccak256("1")
 *   || uint256(1337)
 *   || address(0x0000000000000000000000000000000000000000)
 * )
 *
 * agentTypeHash = keccak256("Agent(address source,bytes32 connectionId)")
 *
 * We precompute these at signer creation for efficiency.
 */

static uint8_t g_domain_separator[32];
static uint8_t g_agent_type_hash[32];
static pthread_once_t g_eip712_once = PTHREAD_ONCE_INIT;

static void compute_eip712_constants(void) {

    uint8_t buf[32 * 5]; /* typeHash + 4 fields */

    /* EIP712Domain type hash */
    const char *domain_type =
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    uint8_t domain_type_hash[32];
    keccak256((const uint8_t *)domain_type, strlen(domain_type), domain_type_hash);

    /* keccak256("Exchange") */
    uint8_t name_hash[32];
    keccak256((const uint8_t *)"Exchange", 8, name_hash);

    /* keccak256("1") */
    uint8_t version_hash[32];
    keccak256((const uint8_t *)"1", 1, version_hash);

    /* uint256(1337) = 0x...0539 */
    uint8_t chain_id[32];
    memset(chain_id, 0, 32);
    chain_id[30] = 0x05;
    chain_id[31] = 0x39;

    /* address(0x0) = 32 bytes of zeros */
    uint8_t contract[32];
    memset(contract, 0, 32);

    /* domainSeparator = keccak256(typeHash || nameHash || versionHash || chainId || contract) */
    memcpy(buf,       domain_type_hash, 32);
    memcpy(buf + 32,  name_hash, 32);
    memcpy(buf + 64,  version_hash, 32);
    memcpy(buf + 96,  chain_id, 32);
    memcpy(buf + 128, contract, 32);
    keccak256(buf, 160, g_domain_separator);

    /* Agent type hash: "Agent(string source,bytes32 connectionId)"
     * IMPORTANT: Hyperliquid uses "string" not "address" for source.
     * Matches the official Python SDK: hyperliquid-dex/hyperliquid-python-sdk */
    const char *agent_type = "Agent(string source,bytes32 connectionId)";
    keccak256((const uint8_t *)agent_type, strlen(agent_type), g_agent_type_hash);
}

/* ── Signer lifecycle ──────────────────────────────────────────────────────── */

hl_signer_t *hl_signer_create(const char *private_key_hex) {
    pthread_once(&g_eip712_once, compute_eip712_constants);

    hl_signer_t *s = calloc(1, sizeof(hl_signer_t));
    if (!s) return NULL;

    /* Parse private key */
    if (hex_decode(private_key_hex, s->privkey, 32) != 0) {
        secure_wipe(s->privkey, 32);
        free(s);
        return NULL;
    }

    /* Create secp256k1 context */
    s->secp_ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );
    if (!s->secp_ctx) {
        secure_wipe(s->privkey, 32);
        free(s);
        return NULL;
    }

    /* Verify private key */
    if (!secp256k1_ec_seckey_verify(s->secp_ctx, s->privkey)) {
        secure_wipe(s->privkey, 32);
        secp256k1_context_destroy(s->secp_ctx);
        free(s);
        return NULL;
    }

    /* Derive public key */
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(s->secp_ctx, &pubkey, s->privkey)) {
        secure_wipe(s->privkey, 32);
        secp256k1_context_destroy(s->secp_ctx);
        free(s);
        return NULL;
    }

    /* Serialize uncompressed (65 bytes: 0x04 || x || y) */
    size_t pub_len = 65;
    secp256k1_ec_pubkey_serialize(s->secp_ctx, s->pubkey_uncompressed,
                                  &pub_len, &pubkey,
                                  SECP256K1_EC_UNCOMPRESSED);

    /* Derive Ethereum address = keccak256(pubkey[1:65])[12:32] */
    uint8_t pub_hash[32];
    keccak256(s->pubkey_uncompressed + 1, 64, pub_hash);
    memcpy(s->address, pub_hash + 12, 20);

    /* Lock private key in memory to prevent swapping to disk */
    mlock(s->privkey, sizeof(s->privkey));

    return s;
}

void hl_signer_destroy(hl_signer_t *signer) {
    if (!signer) return;
    /* Securely wipe private key (explicit_bzero prevents compiler optimization) */
    secure_wipe(signer->privkey, 32);
    if (signer->secp_ctx) {
        secp256k1_context_destroy(signer->secp_ctx);
    }
    free(signer);
}

int hl_signer_get_address(const hl_signer_t *signer, char *out, size_t out_len) {
    if (out_len < 43) return -1; /* "0x" + 40 hex + null */
    out[0] = '0';
    out[1] = 'x';
    hex_encode(signer->address, 20, out + 2, out_len - 2);
    return 0;
}

/* ── Core signing ──────────────────────────────────────────────────────────── */

/*
 * Compute hashStruct(Agent) = keccak256(agentTypeHash || source_padded || connectionId)
 *
 * source is an address: 0x0...0 + one byte (0x61='a' mainnet, 0x62='b' testnet)
 * padded to 32 bytes (left-padded with zeros, address in last 20 bytes)
 */
static void compute_agent_hash(const uint8_t *connection_id, bool is_mainnet,
                                uint8_t *out_hash) {
    uint8_t buf[96]; /* typeHash(32) + source(32) + connectionId(32) */

    /* agentTypeHash */
    memcpy(buf, g_agent_type_hash, 32);

    /* source: EIP-712 encodes "string" as keccak256(bytes_of_string)
     * mainnet source = "a" (0x61), testnet source = "b" (0x62) */
    uint8_t source_byte = is_mainnet ? 0x61 : 0x62;
    keccak256(&source_byte, 1, buf + 32);

    /* connectionId */
    memcpy(buf + 64, connection_id, 32);

    keccak256(buf, 96, out_hash);
}

/*
 * Final EIP-712 hash = keccak256("\x19\x01" || domainSeparator || hashStruct)
 */
static void compute_eip712_hash(const uint8_t *struct_hash, uint8_t *out_hash) {
    uint8_t buf[66]; /* 2 + 32 + 32 */
    buf[0] = 0x19;
    buf[1] = 0x01;
    memcpy(buf + 2, g_domain_separator, 32);
    memcpy(buf + 34, struct_hash, 32);
    keccak256(buf, 66, out_hash);
}

int hl_sign_l1_action(
    const hl_signer_t *signer,
    const uint8_t *msgpack_data,
    size_t msgpack_len,
    uint64_t nonce,
    const char *vault_address,
    bool is_mainnet,
    hl_signature_t *out_sig
) {
    if (!signer || !msgpack_data || !out_sig) return -1;

    /* Step 1: connectionId = keccak256(msgpack || nonce_be_8 || vault_flag [|| vault_20])
     * Matches Python SDK: action_hash() in hyperliquid-python-sdk */
    keccak256_ctx_t ctx;
    keccak256_init(&ctx);
    keccak256_update(&ctx, msgpack_data, msgpack_len);

    /* Nonce as 8-byte BIG-endian (Python: nonce.to_bytes(8, "big")) */
    uint8_t nonce_be[8];
    for (int i = 0; i < 8; i++) {
        nonce_be[i] = (uint8_t)(nonce >> ((7 - i) * 8));
    }
    keccak256_update(&ctx, nonce_be, 8);

    /* Vault address flag: 0x00 = no vault, 0x01 = vault present */
    if (vault_address) {
        uint8_t flag = 0x01;
        keccak256_update(&ctx, &flag, 1);
        uint8_t vault_bytes[20];
        if (hex_decode(vault_address, vault_bytes, 20) == 0) {
            keccak256_update(&ctx, vault_bytes, 20);
        }
    } else {
        uint8_t flag = 0x00;
        keccak256_update(&ctx, &flag, 1);
    }

    uint8_t connection_id[32];
    keccak256_final(&ctx, connection_id);

    /* Step 2: hashStruct(Agent) */
    uint8_t agent_hash[32];
    compute_agent_hash(connection_id, is_mainnet, agent_hash);

    /* Step 3: EIP-712 final hash */
    uint8_t msg_hash[32];
    compute_eip712_hash(agent_hash, msg_hash);

    /* Step 4: Sign with secp256k1 */
    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_sign_recoverable(signer->secp_ctx, &rsig,
                                           msg_hash, signer->privkey,
                                           NULL, NULL)) {
        return -1;
    }

    /* Extract r, s, recid */
    uint8_t sig_bytes[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        signer->secp_ctx, sig_bytes, &recid, &rsig
    );

    memcpy(out_sig->r, sig_bytes, 32);
    memcpy(out_sig->s, sig_bytes + 32, 32);
    out_sig->v = (uint8_t)(27 + recid);

    return 0;
}

int hl_signature_to_hex(const hl_signature_t *sig, char *buf, size_t buf_len) {
    if (buf_len < 133) return -1; /* "0x" + 64 + 64 + 2 + null */

    buf[0] = '0';
    buf[1] = 'x';
    hex_encode(sig->r, 32, buf + 2, buf_len - 2);
    hex_encode(sig->s, 32, buf + 66, buf_len - 66);
    snprintf(buf + 130, buf_len - 130, "%02x", sig->v);
    return 0;
}

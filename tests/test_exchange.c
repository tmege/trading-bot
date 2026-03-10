/*
 * Quick integration test for Phase 2:
 *   1. Keccak256 against known test vector
 *   2. Signer creation from private key
 *   3. REST GET meta (real API call, no auth needed)
 *   4. REST GET allMids (real API call)
 *   5. REST GET l2Book for ETH
 */

#include "exchange/keccak256.h"
#include "exchange/hl_signing.h"
#include "exchange/hl_rest.h"
#include "core/logging.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

/* Test keccak256 against known Ethereum test vector */
static void test_keccak256(void) {
    printf("\n== Keccak256 ==\n");

    /* keccak256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470 */
    uint8_t hash[32];
    keccak256((const uint8_t *)"", 0, hash);

    const uint8_t expected[] = {
        0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
        0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
        0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
        0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70,
    };
    ASSERT(memcmp(hash, expected, 32) == 0, "keccak256('') matches test vector");

    /* keccak256("hello") */
    keccak256((const uint8_t *)"hello", 5, hash);
    const uint8_t expected_hello[] = {
        0x1c, 0x8a, 0xff, 0x95, 0x06, 0x85, 0xc2, 0xed,
        0x4b, 0xc3, 0x17, 0x4f, 0x34, 0x72, 0x28, 0x7b,
        0x56, 0xd9, 0x51, 0x7b, 0x9c, 0x94, 0x81, 0x27,
        0x31, 0x9a, 0x09, 0xa7, 0xa3, 0x6d, 0xea, 0xc8,
    };
    ASSERT(memcmp(hash, expected_hello, 32) == 0, "keccak256('hello') matches test vector");
}

/* Test signer: derive address from known private key */
static void test_signer(void) {
    printf("\n== Signer ==\n");

    /* Well-known test key: private key = 1
     * Expected address: 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf */
    hl_signer_t *s = hl_signer_create(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );
    ASSERT(s != NULL, "signer created from private key");

    if (s) {
        char addr[44];
        hl_signer_get_address(s, addr, sizeof(addr));
        printf("  Derived address: %s\n", addr);

        /* Compare lowercase (our implementation doesn't checksum) */
        ASSERT(strncasecmp(addr, "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf", 42) == 0,
               "address matches expected for privkey=1");

        /* Test signing (just verify it doesn't crash) */
        uint8_t dummy_msgpack[] = {0x82, 0xa4, 0x74, 0x79, 0x70, 0x65};
        hl_signature_t sig;
        int rc = hl_sign_l1_action(s, dummy_msgpack, sizeof(dummy_msgpack),
                                    1234567890123ULL, NULL, true, &sig);
        ASSERT(rc == 0, "sign_l1_action succeeds");
        ASSERT(sig.v == 27 || sig.v == 28, "signature v is 27 or 28");

        char sig_hex[134];
        hl_signature_to_hex(&sig, sig_hex, sizeof(sig_hex));
        printf("  Signature: %.20s...\n", sig_hex);
        ASSERT(strlen(sig_hex) == 132, "signature hex is 132 chars");

        hl_signer_destroy(s);
    }
}

/* Test REST: fetch real market data (no auth needed) */
static void test_rest_info(void) {
    printf("\n== REST Info (live API) ==\n");

    hl_rest_t *rest = hl_rest_create("https://api.hyperliquid.xyz", NULL, true);
    ASSERT(rest != NULL, "REST client created");
    if (!rest) return;

    /* Get meta */
    tb_asset_meta_t assets[TB_MAX_ASSETS];
    int n_assets = 0;
    int rc = hl_rest_get_meta(rest, assets, &n_assets);
    ASSERT(rc == 0, "get_meta succeeded");
    ASSERT(n_assets > 0, "got assets from meta");
    if (n_assets > 0) {
        printf("  Found %d assets. First: %s (id=%d, sz_dec=%d)\n",
               n_assets, assets[0].name, assets[0].asset_id, assets[0].sz_decimals);
    }

    /* Get all mids */
    tb_mid_t mids[TB_MAX_ASSETS];
    int n_mids = 0;
    rc = hl_rest_get_all_mids(rest, mids, &n_mids);
    ASSERT(rc == 0, "get_all_mids succeeded");
    ASSERT(n_mids > 0, "got mid prices");

    /* Find ETH mid price */
    for (int i = 0; i < n_mids; i++) {
        if (strcmp(mids[i].coin, "ETH") == 0) {
            double eth_price = tb_decimal_to_double(mids[i].mid);
            printf("  ETH mid: $%.2f\n", eth_price);
            ASSERT(eth_price > 100.0 && eth_price < 100000.0, "ETH price in sane range");
            break;
        }
    }

    /* Get ETH L2 book */
    tb_book_t book;
    memset(&book, 0, sizeof(book));
    rc = hl_rest_get_l2_book(rest, "ETH", &book);
    ASSERT(rc == 0, "get_l2_book(ETH) succeeded");
    ASSERT(book.n_bids > 0, "ETH book has bids");
    ASSERT(book.n_asks > 0, "ETH book has asks");
    if (book.n_bids > 0 && book.n_asks > 0) {
        printf("  ETH book: best bid=$%.2f, best ask=$%.2f, %d/%d levels\n",
               tb_decimal_to_double(book.bids[0].px),
               tb_decimal_to_double(book.asks[0].px),
               book.n_bids, book.n_asks);
    }

    hl_rest_destroy(rest);
}

int main(void) {
    tb_log_init("./logs", 1); /* INFO level */

    test_keccak256();
    test_signer();
    test_rest_info();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}

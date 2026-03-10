#ifndef TB_MACRO_FETCHER_H
#define TB_MACRO_FETCHER_H

#include <stdbool.h>
#include <stdint.h>

/* ── Macro data snapshot ────────────────────────────────────────────────── */
typedef struct {
    /* Traditional markets */
    double sp500;
    double gold;
    double dxy;              /* US Dollar Index */

    /* Crypto macro */
    double btc_price;
    double btc_dominance;    /* % */
    double eth_btc;          /* ETH/BTC ratio */
    double total2_mcap;      /* Total2 marketcap (excl BTC) in billions */

    /* Metadata */
    int64_t last_update_ms;
    bool    valid;
} tb_macro_data_t;

typedef struct tb_macro_fetcher tb_macro_fetcher_t;

/* Forward declaration */
typedef struct hl_rest hl_rest_t;

/* Create fetcher. api_key can be NULL for free-tier APIs. */
tb_macro_fetcher_t *tb_macro_fetcher_create(const char *api_key);
void                tb_macro_fetcher_destroy(tb_macro_fetcher_t *f);

/* Set Hyperliquid REST client for crypto prices (optional, falls back to CoinGecko). */
void tb_macro_fetcher_set_hl_rest(tb_macro_fetcher_t *f, hl_rest_t *rest);

/* Fetch all macro data (blocking HTTP calls). Returns 0 on success. */
int tb_macro_fetcher_refresh(tb_macro_fetcher_t *f);

/* Get latest snapshot (thread-safe read). */
tb_macro_data_t tb_macro_fetcher_get(const tb_macro_fetcher_t *f);

#endif /* TB_MACRO_FETCHER_H */

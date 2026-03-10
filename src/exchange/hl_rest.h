#ifndef TB_HL_REST_H
#define TB_HL_REST_H

#include "core/types.h"
#include "exchange/hl_signing.h"
#include <stdbool.h>

typedef struct hl_rest hl_rest_t;

hl_rest_t *hl_rest_create(const char *base_url, hl_signer_t *signer, bool is_mainnet);
void       hl_rest_destroy(hl_rest_t *rest);

/* ── Info endpoints (POST /info, no auth) ─────────────────────────────────── */
int hl_rest_get_meta(hl_rest_t *rest, tb_asset_meta_t *out_assets, int *out_count);
int hl_rest_get_all_mids(hl_rest_t *rest, tb_mid_t *out_mids, int *out_count);
int hl_rest_get_l2_book(hl_rest_t *rest, const char *coin, tb_book_t *out_book);
int hl_rest_get_candles(hl_rest_t *rest, const char *coin, const char *interval,
                        int64_t start_ms, int64_t end_ms,
                        tb_candle_t *out_candles, int *out_count);
int hl_rest_get_account(hl_rest_t *rest, const char *user_addr, tb_account_t *out);
int hl_rest_get_open_orders(hl_rest_t *rest, const char *user_addr,
                            tb_order_t *out_orders, int *out_count);
int hl_rest_get_user_fills(hl_rest_t *rest, const char *user_addr,
                           tb_fill_t *out_fills, int *out_count);

/* ── Exchange endpoints (POST /exchange, EIP-712 auth) ────────────────────── */
int hl_rest_place_order(hl_rest_t *rest,
                        const tb_order_request_t *order,
                        tb_grouping_t grouping,
                        uint64_t *out_oid);

int hl_rest_place_orders(hl_rest_t *rest,
                         const tb_order_request_t *orders, int n_orders,
                         tb_grouping_t grouping,
                         uint64_t *out_oids, int *out_n_filled);

int hl_rest_cancel_order(hl_rest_t *rest, uint32_t asset, uint64_t oid);

int hl_rest_cancel_orders(hl_rest_t *rest,
                          const uint32_t *assets, const uint64_t *oids,
                          int n_cancels);

int hl_rest_update_leverage(hl_rest_t *rest,
                            uint32_t asset, int leverage, bool is_cross);

#endif /* TB_HL_REST_H */

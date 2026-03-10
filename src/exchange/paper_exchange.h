#ifndef TB_PAPER_EXCHANGE_H
#define TB_PAPER_EXCHANGE_H

#include "core/types.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct tb_paper_exchange tb_paper_exchange_t;

/* Callback when a simulated fill occurs. */
typedef void (*tb_paper_fill_cb)(const tb_fill_t *fill, void *user_data);

/* ── Configuration ─────────────────────────────────────────────────────── */
typedef struct {
    double initial_balance;
    double maker_fee_rate;   /* default 0.0002 */
    double taker_fee_rate;   /* default 0.0005 */
} tb_paper_config_t;

/* ── API ───────────────────────────────────────────────────────────────── */

tb_paper_exchange_t *tb_paper_create(const tb_paper_config_t *cfg);
void                 tb_paper_destroy(tb_paper_exchange_t *pe);

/* Set fill callback. */
void tb_paper_set_fill_cb(tb_paper_exchange_t *pe,
                          tb_paper_fill_cb cb, void *user_data);

/* Feed real market mid price. Call on every WS allMids update.
   This triggers order matching. */
void tb_paper_feed_mid(tb_paper_exchange_t *pe,
                       const char *coin, double mid_price);

/* ── Order interface (same semantics as hl_rest) ───────────────────────── */

int tb_paper_place_order(tb_paper_exchange_t *pe,
                         const tb_order_request_t *order,
                         uint64_t *out_oid);

int tb_paper_cancel_order(tb_paper_exchange_t *pe,
                          uint32_t asset, uint64_t oid);

/* ── Query interface ───────────────────────────────────────────────────── */

int tb_paper_get_open_orders(tb_paper_exchange_t *pe,
                             tb_order_t *out_orders, int *out_count);

int tb_paper_get_positions(tb_paper_exchange_t *pe,
                           tb_position_t *out_positions, int *out_count);

double tb_paper_get_account_value(tb_paper_exchange_t *pe);
double tb_paper_get_daily_pnl(tb_paper_exchange_t *pe);

/* Reset daily P&L (call at midnight UTC). */
void tb_paper_reset_daily(tb_paper_exchange_t *pe);

#endif /* TB_PAPER_EXCHANGE_H */

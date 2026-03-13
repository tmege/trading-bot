#ifndef TB_ORDER_MANAGER_H
#define TB_ORDER_MANAGER_H

#include "core/types.h"
#include "exchange/hl_rest.h"
#include "exchange/hl_ws.h"
#include "exchange/paper_exchange.h"
#include "exchange/position_tracker.h"
#include <sqlite3.h>
#include <stdbool.h>

/* Forward declaration */
typedef struct tb_risk_mgr tb_risk_mgr_t;

typedef struct tb_order_mgr tb_order_mgr_t;

/* Fill callback: notified on every fill with strategy name */
typedef void (*tb_on_fill_cb)(const tb_fill_t *fill, const char *strategy,
                               void *userdata);

/* Order submission with strategy attribution */
typedef struct {
    tb_order_request_t order;
    char               strategy_name[64];
} tb_order_submit_t;

tb_order_mgr_t *tb_order_mgr_create(hl_rest_t *rest, hl_ws_t *ws,
                                     tb_risk_mgr_t *risk,
                                     tb_position_tracker_t *pos_tracker,
                                     sqlite3 *db);
void            tb_order_mgr_destroy(tb_order_mgr_t *mgr);

/* Load asset metadata from exchange (call before start in live mode) */
int  tb_order_mgr_load_meta(tb_order_mgr_t *mgr);

/* Start: subscribe to WS updates, begin reconciliation loop */
int  tb_order_mgr_start(tb_order_mgr_t *mgr, const char *user_addr);
void tb_order_mgr_stop(tb_order_mgr_t *mgr);

/* Submit order (goes through risk check first) */
int tb_order_mgr_submit(tb_order_mgr_t *mgr, const tb_order_submit_t *submit,
                        uint64_t *out_oid);

/* Cancel */
int tb_order_mgr_cancel(tb_order_mgr_t *mgr, uint32_t asset, uint64_t oid);
int tb_order_mgr_cancel_by_coin(tb_order_mgr_t *mgr, const char *coin,
                                 uint64_t oid);
int tb_order_mgr_cancel_all_coin(tb_order_mgr_t *mgr, const char *coin);
int tb_order_mgr_cancel_all_exchange_coin(tb_order_mgr_t *mgr, const char *coin);
int tb_order_mgr_cancel_all_exchange_coin_strategy(tb_order_mgr_t *mgr,
                                                     const char *coin,
                                                     const char *strategy);

/* Query open orders for a coin (thread-safe) */
int tb_order_mgr_get_open_orders(tb_order_mgr_t *mgr, const char *coin,
                                  tb_order_t *out, int *count);

/* Get total number of open orders */
int tb_order_mgr_open_order_count(tb_order_mgr_t *mgr);

/* Set paper exchange (routes orders through paper instead of REST) */
void tb_order_mgr_set_paper(tb_order_mgr_t *mgr, tb_paper_exchange_t *paper);

/* Register fill callback */
void tb_order_mgr_set_fill_callback(tb_order_mgr_t *mgr, tb_on_fill_cb cb,
                                     void *userdata);

/* Force reconciliation with exchange (REST fetch) */
int tb_order_mgr_reconcile(tb_order_mgr_t *mgr);

/* WS event handlers (called by engine from WS callbacks) */
void tb_order_mgr_handle_ws_fills(tb_order_mgr_t *mgr,
                                   const tb_fill_t *fills, int n_fills);
void tb_order_mgr_handle_ws_orders(tb_order_mgr_t *mgr,
                                    const tb_order_t *orders, int n_orders);

#endif /* TB_ORDER_MANAGER_H */

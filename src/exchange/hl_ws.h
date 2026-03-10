#ifndef TB_HL_WS_H
#define TB_HL_WS_H

#include "core/types.h"
#include <stdbool.h>

typedef struct hl_ws hl_ws_t;

/* Callback types */
typedef void (*hl_ws_mids_cb)(const tb_mid_t *mids, int n_mids, void *userdata);
typedef void (*hl_ws_book_cb)(const tb_book_t *book, void *userdata);
typedef void (*hl_ws_candle_cb)(const char *coin, const tb_candle_t *candle, void *userdata);
typedef void (*hl_ws_order_cb)(const tb_order_t *orders, int n_orders, void *userdata);
typedef void (*hl_ws_fill_cb)(const tb_fill_t *fills, int n_fills, void *userdata);

typedef struct {
    hl_ws_mids_cb   on_mids;
    hl_ws_book_cb   on_book;
    hl_ws_candle_cb on_candle;
    hl_ws_order_cb  on_order_update;
    hl_ws_fill_cb   on_fill;
    void           *userdata;
} hl_ws_callbacks_t;

hl_ws_t *hl_ws_create(const char *ws_url, const hl_ws_callbacks_t *cbs);
void     hl_ws_destroy(hl_ws_t *ws);

/* Connect and start event loop thread */
int  hl_ws_connect(hl_ws_t *ws);
void hl_ws_disconnect(hl_ws_t *ws);

/* Subscriptions */
int hl_ws_subscribe_all_mids(hl_ws_t *ws);
int hl_ws_subscribe_l2_book(hl_ws_t *ws, const char *coin);
int hl_ws_subscribe_candle(hl_ws_t *ws, const char *coin, const char *interval);
int hl_ws_subscribe_order_updates(hl_ws_t *ws, const char *user_addr);
int hl_ws_subscribe_user_fills(hl_ws_t *ws, const char *user_addr);

bool hl_ws_is_connected(const hl_ws_t *ws);

#endif /* TB_HL_WS_H */

#ifndef TB_HL_JSON_H
#define TB_HL_JSON_H

#include "core/types.h"
#include <yyjson.h>

/* Parse meta response → asset list */
int hl_json_parse_meta(yyjson_val *root, tb_asset_meta_t *assets, int *count);

/* Parse allMids response → mid prices */
int hl_json_parse_all_mids(yyjson_val *root, tb_mid_t *mids, int *count);

/* Parse l2Book response → order book */
int hl_json_parse_l2_book(yyjson_val *root, tb_book_t *book);

/* Parse candle response → candle array (max_count = buffer capacity) */
int hl_json_parse_candles(yyjson_val *root, tb_candle_t *candles, int *count,
                           int max_count);

/* Parse clearinghouseState response → account */
int hl_json_parse_account(yyjson_val *root, tb_account_t *account);

/* Parse openOrders response → orders (max_count = buffer capacity) */
int hl_json_parse_orders(yyjson_val *root, tb_order_t *orders, int *count,
                          int max_count);

/* Parse userFills response → fills (max_count = buffer capacity) */
int hl_json_parse_fills(yyjson_val *root, tb_fill_t *fills, int *count,
                         int max_count);

/* Parse exchange response (order placement result) */
int hl_json_parse_exchange_response(yyjson_val *root,
                                     uint64_t *oids, int max_oids,
                                     int *n_filled,
                                     char *err_msg, size_t err_len);

/* REST fill info — returned when an order fills immediately */
typedef struct {
    uint64_t oid;
    double   avg_px;
    double   total_sz;
    bool     filled;    /* true if this order was filled (not resting/error) */
} hl_rest_fill_info_t;

/* Parse exchange response with fill details */
int hl_json_parse_exchange_response_ex(yyjson_val *root,
                                        hl_rest_fill_info_t *fill_infos,
                                        int max_orders,
                                        int *n_filled,
                                        char *err_msg, size_t err_len);

/* Build JSON body for POST /exchange request */
int hl_json_build_exchange_body(const char *action_json, uint64_t nonce,
                                 const char *sig_hex, const char *vault_addr,
                                 char **out_json);

#endif /* TB_HL_JSON_H */

#ifndef TB_TYPES_H
#define TB_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Fixed-point decimal for financial precision ─────────────────────────── */
/* value = mantissa / 10^scale. Never use float/double for prices or sizes. */
typedef struct {
    int64_t  mantissa;
    uint8_t  scale;
} tb_decimal_t;

/* ── Enums ───────────────────────────────────────────────────────────────── */

typedef enum {
    TB_SIDE_BUY  = 0,
    TB_SIDE_SELL = 1,
} tb_side_t;

typedef enum {
    TB_TIF_GTC = 0,    /* Good til cancel */
    TB_TIF_IOC = 1,    /* Immediate or cancel */
    TB_TIF_ALO = 2,    /* Add liquidity only (post-only) */
} tb_tif_t;

typedef enum {
    TB_ORDER_LIMIT   = 0,
    TB_ORDER_TRIGGER = 1,
} tb_order_type_t;

typedef enum {
    TB_TPSL_TP = 0,
    TB_TPSL_SL = 1,
} tb_tpsl_t;

typedef enum {
    TB_GROUP_NA          = 0,
    TB_GROUP_NORMAL_TPSL = 1,
    TB_GROUP_POS_TPSL    = 2,
} tb_grouping_t;

/* ── Order request (strategy → order manager) ────────────────────────────── */
typedef struct {
    uint32_t        asset;
    char            coin[16];   /* coin name for position lookup */
    tb_side_t       side;
    tb_decimal_t    price;
    tb_decimal_t    size;
    bool            reduce_only;
    tb_order_type_t type;
    tb_tif_t        tif;
    /* Trigger fields (only when type == TB_ORDER_TRIGGER) */
    bool            is_market;
    tb_decimal_t    trigger_px;
    tb_tpsl_t       tpsl;
    /* Optional client order id (max 32 hex chars) */
    char            cloid[34];
    tb_grouping_t   grouping;
} tb_order_request_t;

/* ── Live order (from exchange) ──────────────────────────────────────────── */
typedef struct {
    uint64_t     oid;
    uint32_t     asset;
    char         coin[16];
    tb_side_t    side;
    tb_decimal_t limit_px;
    tb_decimal_t sz;
    tb_decimal_t orig_sz;
    int64_t      timestamp_ms;
    bool         reduce_only;
    tb_tif_t     tif;
    char         cloid[34];
} tb_order_t;

/* ── Fill ─────────────────────────────────────────────────────────────────── */
typedef struct {
    char         coin[16];
    tb_decimal_t px;
    tb_decimal_t sz;
    tb_side_t    side;
    int64_t      time_ms;
    tb_decimal_t closed_pnl;
    tb_decimal_t fee;
    uint64_t     oid;
    uint64_t     tid;
    bool         crossed;   /* true = taker fill */
    char         hash[68];
} tb_fill_t;

/* ── Position ─────────────────────────────────────────────────────────────── */
typedef struct {
    char         coin[16];
    uint32_t     asset;
    tb_decimal_t size;          /* negative = short */
    tb_decimal_t entry_px;
    tb_decimal_t unrealized_pnl;
    tb_decimal_t realized_pnl;
    tb_decimal_t liquidation_px;
    tb_decimal_t margin_used;
    int          leverage;
    bool         is_cross;
} tb_position_t;

/* ── L2 Book ──────────────────────────────────────────────────────────────── */
#define TB_MAX_BOOK_LEVELS 20

typedef struct {
    tb_decimal_t px;
    tb_decimal_t sz;
    int          n_orders;
} tb_book_level_t;

typedef struct {
    char            coin[16];
    tb_book_level_t bids[TB_MAX_BOOK_LEVELS];
    tb_book_level_t asks[TB_MAX_BOOK_LEVELS];
    int             n_bids;
    int             n_asks;
    int64_t         timestamp_ms;
} tb_book_t;

/* ── Candle ───────────────────────────────────────────────────────────────── */
typedef struct {
    int64_t      time_open;
    int64_t      time_close;
    tb_decimal_t open;
    tb_decimal_t high;
    tb_decimal_t low;
    tb_decimal_t close;
    tb_decimal_t volume;
    int          n_trades;
} tb_candle_t;

/* ── Mid price ────────────────────────────────────────────────────────────── */
typedef struct {
    char         coin[16];
    tb_decimal_t mid;
} tb_mid_t;

/* ── Account summary ──────────────────────────────────────────────────────── */
#define TB_MAX_POSITIONS 64

typedef struct {
    tb_decimal_t  account_value;
    tb_decimal_t  total_margin_used;
    tb_decimal_t  total_unrealized_pnl;
    tb_decimal_t  withdrawable;
    tb_position_t positions[TB_MAX_POSITIONS];
    int           n_positions;
} tb_account_t;

/* ── Asset metadata ───────────────────────────────────────────────────────── */
#define TB_MAX_ASSETS 512

typedef struct {
    char     name[16];
    uint32_t asset_id;
    int      sz_decimals;
} tb_asset_meta_t;

/* ── Decimal helpers ──────────────────────────────────────────────────────── */

/* Create decimal from double (for config/display only, not for trading math) */
static inline tb_decimal_t tb_decimal_from_double(double val, uint8_t scale) {
    tb_decimal_t d;
    d.scale = scale;
    double mult = 1.0;
    for (uint8_t i = 0; i < scale; i++) mult *= 10.0;
    d.mantissa = (int64_t)(val * mult + (val >= 0 ? 0.5 : -0.5));
    return d;
}

/* Convert decimal to double (for display/logging) */
static inline double tb_decimal_to_double(tb_decimal_t d) {
    double div = 1.0;
    for (uint8_t i = 0; i < d.scale; i++) div *= 10.0;
    return (double)d.mantissa / div;
}

/* Convert decimal to string (for API calls) */
int tb_decimal_to_str(tb_decimal_t d, char *buf, size_t buf_len);

/* Parse decimal from string */
tb_decimal_t tb_decimal_from_str(const char *str);

/* Arithmetic */
tb_decimal_t tb_decimal_add(tb_decimal_t a, tb_decimal_t b);
tb_decimal_t tb_decimal_sub(tb_decimal_t a, tb_decimal_t b);
tb_decimal_t tb_decimal_mul(tb_decimal_t a, tb_decimal_t b);
tb_decimal_t tb_decimal_div(tb_decimal_t a, tb_decimal_t b);
int          tb_decimal_cmp(tb_decimal_t a, tb_decimal_t b);
bool         tb_decimal_is_zero(tb_decimal_t d);
tb_decimal_t tb_decimal_abs(tb_decimal_t d);
tb_decimal_t tb_decimal_neg(tb_decimal_t d);

#endif /* TB_TYPES_H */

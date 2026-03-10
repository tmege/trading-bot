#include "exchange/hl_json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Safe helpers */
static const char *get_str(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_get_str(v);
}

__attribute__((unused))
static double get_num(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_is_num(v) ? yyjson_get_num(v) : 0.0;
}

static int64_t get_int(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_is_int(v) ? yyjson_get_sint(v) : 0;
}

static bool get_bool(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : false;
}

static void safe_strcpy(char *dst, size_t dst_len, const char *src) {
    if (src) snprintf(dst, dst_len, "%s", src);
    else dst[0] = '\0';
}

/* ── Parse meta (metaAndAssetCtxs) ──────────────────────────────────────── */
int hl_json_parse_meta(yyjson_val *root, tb_asset_meta_t *assets, int *count) {
    /* Response: [{ "universe": [...] }, [...]] */
    yyjson_val *meta = yyjson_arr_get_first(root);
    if (!meta) return -1;

    yyjson_val *universe = yyjson_obj_get(meta, "universe");
    if (!universe || !yyjson_is_arr(universe)) return -1;

    int n = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(universe, idx, max, item) {
        if (n >= TB_MAX_ASSETS) break;
        safe_strcpy(assets[n].name, sizeof(assets[n].name),
                    get_str(item, "name"));
        assets[n].asset_id = (uint32_t)idx;
        assets[n].sz_decimals = (int)get_int(item, "szDecimals");
        n++;
    }
    *count = n;
    return 0;
}

/* ── Parse allMids ──────────────────────────────────────────────────────── */
int hl_json_parse_all_mids(yyjson_val *root, tb_mid_t *mids, int *count) {
    /* Response: { "coin": "price", ... } */
    if (!yyjson_is_obj(root)) return -1;

    int n = 0;
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(root, idx, max, key, val) {
        if (n >= TB_MAX_ASSETS) break;
        const char *coin = yyjson_get_str(key);
        const char *mid_str = yyjson_get_str(val);
        if (coin && mid_str) {
            safe_strcpy(mids[n].coin, sizeof(mids[n].coin), coin);
            mids[n].mid = tb_decimal_from_str(mid_str);
            n++;
        }
    }
    *count = n;
    return 0;
}

/* ── Parse l2Book ───────────────────────────────────────────────────────── */
int hl_json_parse_l2_book(yyjson_val *root, tb_book_t *book) {
    /* Response: { "levels": [[bids], [asks]], "coin": "ETH" } */
    yyjson_val *levels = yyjson_obj_get(root, "levels");
    if (!levels || !yyjson_is_arr(levels)) return -1;

    safe_strcpy(book->coin, sizeof(book->coin), get_str(root, "coin"));

    yyjson_val *bids = yyjson_arr_get(levels, 0);
    yyjson_val *asks = yyjson_arr_get(levels, 1);

    book->n_bids = 0;
    if (bids && yyjson_is_arr(bids)) {
        size_t idx, max;
        yyjson_val *lvl;
        yyjson_arr_foreach(bids, idx, max, lvl) {
            if (book->n_bids >= TB_MAX_BOOK_LEVELS) break;
            yyjson_val *px_val = yyjson_obj_get(lvl, "px");
            yyjson_val *sz_val = yyjson_obj_get(lvl, "sz");
            yyjson_val *n_val  = yyjson_obj_get(lvl, "n");
            const char *px_str = yyjson_get_str(px_val);
            const char *sz_str = yyjson_get_str(sz_val);
            if (px_str && sz_str) {
                book->bids[book->n_bids].px = tb_decimal_from_str(px_str);
                book->bids[book->n_bids].sz = tb_decimal_from_str(sz_str);
                book->bids[book->n_bids].n_orders = n_val ? (int)yyjson_get_int(n_val) : 0;
                book->n_bids++;
            }
        }
    }

    book->n_asks = 0;
    if (asks && yyjson_is_arr(asks)) {
        size_t idx, max;
        yyjson_val *lvl;
        yyjson_arr_foreach(asks, idx, max, lvl) {
            if (book->n_asks >= TB_MAX_BOOK_LEVELS) break;
            yyjson_val *px_val = yyjson_obj_get(lvl, "px");
            yyjson_val *sz_val = yyjson_obj_get(lvl, "sz");
            yyjson_val *n_val  = yyjson_obj_get(lvl, "n");
            const char *px_str = yyjson_get_str(px_val);
            const char *sz_str = yyjson_get_str(sz_val);
            if (px_str && sz_str) {
                book->asks[book->n_asks].px = tb_decimal_from_str(px_str);
                book->asks[book->n_asks].sz = tb_decimal_from_str(sz_str);
                book->asks[book->n_asks].n_orders = n_val ? (int)yyjson_get_int(n_val) : 0;
                book->n_asks++;
            }
        }
    }

    return 0;
}

/* ── Parse candles ──────────────────────────────────────────────────────── */
int hl_json_parse_candles(yyjson_val *root, tb_candle_t *candles, int *count) {
    if (!yyjson_is_arr(root)) return -1;

    int n = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        if (n >= 5000) break; /* API max */
        candles[n].time_open  = get_int(item, "t");
        candles[n].time_close = get_int(item, "T");
        const char *o = get_str(item, "o");
        const char *h = get_str(item, "h");
        const char *l = get_str(item, "l");
        const char *c = get_str(item, "c");
        const char *v = get_str(item, "v");
        if (o) candles[n].open   = tb_decimal_from_str(o);
        if (h) candles[n].high   = tb_decimal_from_str(h);
        if (l) candles[n].low    = tb_decimal_from_str(l);
        if (c) candles[n].close  = tb_decimal_from_str(c);
        if (v) candles[n].volume = tb_decimal_from_str(v);
        candles[n].n_trades = (int)get_int(item, "n");
        n++;
    }
    *count = n;
    return 0;
}

/* ── Parse clearinghouseState (account) ─────────────────────────────────── */
int hl_json_parse_account(yyjson_val *root, tb_account_t *account) {
    memset(account, 0, sizeof(*account));

    yyjson_val *margin = yyjson_obj_get(root, "marginSummary");
    if (margin) {
        const char *av = get_str(margin, "accountValue");
        const char *mu = get_str(margin, "totalMarginUsed");
        const char *upnl_str = get_str(margin, "totalNtlPos");
        if (av) account->account_value = tb_decimal_from_str(av);
        if (mu) account->total_margin_used = tb_decimal_from_str(mu);
        if (upnl_str) account->total_unrealized_pnl = tb_decimal_from_str(upnl_str);
    }

    yyjson_val *positions = yyjson_obj_get(root, "assetPositions");
    if (positions && yyjson_is_arr(positions)) {
        int n = 0;
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(positions, idx, max, item) {
            if (n >= TB_MAX_POSITIONS) break;
            yyjson_val *pos = yyjson_obj_get(item, "position");
            if (!pos) continue;

            const char *coin = get_str(pos, "coin");
            const char *sz   = get_str(pos, "szi");
            if (!coin || !sz) continue;

            /* Skip zero positions */
            tb_decimal_t size = tb_decimal_from_str(sz);
            if (tb_decimal_is_zero(size)) continue;

            safe_strcpy(account->positions[n].coin,
                        sizeof(account->positions[n].coin), coin);
            account->positions[n].size = size;

            const char *ep = get_str(pos, "entryPx");
            if (ep) account->positions[n].entry_px = tb_decimal_from_str(ep);

            const char *upnl = get_str(pos, "unrealizedPnl");
            if (upnl) account->positions[n].unrealized_pnl = tb_decimal_from_str(upnl);

            const char *liq = get_str(pos, "liquidationPx");
            if (liq) account->positions[n].liquidation_px = tb_decimal_from_str(liq);

            yyjson_val *lev = yyjson_obj_get(pos, "leverage");
            if (lev) {
                const char *lev_type = get_str(lev, "type");
                int lev_val = (int)get_int(lev, "value");
                account->positions[n].leverage = lev_val;
                account->positions[n].is_cross =
                    (lev_type && strcmp(lev_type, "cross") == 0);
            }

            n++;
        }
        account->n_positions = n;
    }

    return 0;
}

/* ── Parse open orders ──────────────────────────────────────────────────── */
int hl_json_parse_orders(yyjson_val *root, tb_order_t *orders, int *count) {
    if (!yyjson_is_arr(root)) return -1;

    int n = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        if (n >= 1000) break;

        orders[n].oid = (uint64_t)get_int(item, "oid");
        safe_strcpy(orders[n].coin, sizeof(orders[n].coin), get_str(item, "coin"));

        const char *side = get_str(item, "side");
        orders[n].side = (side && strcmp(side, "B") == 0) ? TB_SIDE_BUY : TB_SIDE_SELL;

        const char *px = get_str(item, "limitPx");
        if (px) orders[n].limit_px = tb_decimal_from_str(px);

        const char *sz = get_str(item, "sz");
        if (sz) orders[n].sz = tb_decimal_from_str(sz);

        const char *orig = get_str(item, "origSz");
        if (orig) orders[n].orig_sz = tb_decimal_from_str(orig);

        orders[n].timestamp_ms = get_int(item, "timestamp");
        orders[n].reduce_only = get_bool(item, "reduceOnly");

        safe_strcpy(orders[n].cloid, sizeof(orders[n].cloid), get_str(item, "cloid"));
        n++;
    }
    *count = n;
    return 0;
}

/* ── Parse fills ────────────────────────────────────────────────────────── */
int hl_json_parse_fills(yyjson_val *root, tb_fill_t *fills, int *count) {
    if (!yyjson_is_arr(root)) return -1;

    int n = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        if (n >= 2000) break;

        safe_strcpy(fills[n].coin, sizeof(fills[n].coin), get_str(item, "coin"));

        const char *px = get_str(item, "px");
        if (px) fills[n].px = tb_decimal_from_str(px);

        const char *sz = get_str(item, "sz");
        if (sz) fills[n].sz = tb_decimal_from_str(sz);

        const char *side = get_str(item, "side");
        fills[n].side = (side && strcmp(side, "B") == 0) ? TB_SIDE_BUY : TB_SIDE_SELL;

        fills[n].time_ms = get_int(item, "time");

        const char *cpnl = get_str(item, "closedPnl");
        if (cpnl) fills[n].closed_pnl = tb_decimal_from_str(cpnl);

        const char *fee = get_str(item, "fee");
        if (fee) fills[n].fee = tb_decimal_from_str(fee);

        fills[n].oid = (uint64_t)get_int(item, "oid");
        fills[n].tid = (uint64_t)get_int(item, "tid");
        fills[n].crossed = get_bool(item, "crossed");

        safe_strcpy(fills[n].hash, sizeof(fills[n].hash), get_str(item, "hash"));
        n++;
    }
    *count = n;
    return 0;
}

/* ── Parse exchange response ────────────────────────────────────────────── */
int hl_json_parse_exchange_response(yyjson_val *root,
                                     uint64_t *oids, int max_oids,
                                     int *n_filled,
                                     char *err_msg, size_t err_len) {
    const char *status = get_str(root, "status");
    if (!status || strcmp(status, "ok") != 0) {
        /* Error response */
        const char *response = get_str(root, "response");
        if (response && err_msg) {
            snprintf(err_msg, err_len, "%s", response);
        }
        return -1;
    }

    /* Parse statuses array */
    yyjson_val *response = yyjson_obj_get(root, "response");
    if (!response) return 0;

    yyjson_val *data = yyjson_obj_get(response, "data");
    if (!data) return 0;

    yyjson_val *statuses = yyjson_obj_get(data, "statuses");
    if (!statuses || !yyjson_is_arr(statuses)) return 0;

    int filled = 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(statuses, idx, max, item) {
        /* Resting order: { "resting": { "oid": N } } */
        yyjson_val *resting = yyjson_obj_get(item, "resting");
        if (resting) {
            if (oids && (int)idx < max_oids) oids[idx] = (uint64_t)get_int(resting, "oid");
            continue;
        }
        /* Filled order: { "filled": { "totalSz": "..", "avgPx": "..", "oid": N } } */
        yyjson_val *fld = yyjson_obj_get(item, "filled");
        if (fld) {
            if (oids && (int)idx < max_oids) oids[idx] = (uint64_t)get_int(fld, "oid");
            filled++;
            continue;
        }
        /* Error for this specific order */
        yyjson_val *error = yyjson_obj_get(item, "error");
        if (error && err_msg && err_len > 0) {
            const char *e = yyjson_get_str(error);
            if (e) snprintf(err_msg, err_len, "%s", e);
        }
    }

    if (n_filled) *n_filled = filled;
    return 0;
}

/* ── Build exchange request body ────────────────────────────────────────── */
int hl_json_build_exchange_body(const char *action_json, uint64_t nonce,
                                 const char *sig_hex, const char *vault_addr,
                                 char **out_json) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* Parse action JSON into mutable val */
    yyjson_doc *action_doc = yyjson_read(action_json, strlen(action_json), 0);
    if (action_doc) {
        yyjson_mut_val *action = yyjson_val_mut_copy(doc, yyjson_doc_get_root(action_doc));
        yyjson_mut_obj_add_val(doc, root, "action", action);
        yyjson_doc_free(action_doc);
    }

    yyjson_mut_obj_add_uint(doc, root, "nonce", nonce);

    /* Signature object: { "r": "0x...", "s": "0x...", "v": N } */
    yyjson_mut_val *sig_obj = yyjson_mut_obj(doc);
    /* Parse r, s, v from hex string "0x" + r(64) + s(64) + v(2) */
    if (sig_hex && strlen(sig_hex) >= 132) {
        char r_hex[67], s_hex[67];
        snprintf(r_hex, sizeof(r_hex), "0x%.64s", sig_hex + 2);
        snprintf(s_hex, sizeof(s_hex), "0x%.64s", sig_hex + 66);
        int v = 0;
        sscanf(sig_hex + 130, "%2x", (unsigned int *)&v);

        yyjson_mut_obj_add_str(doc, sig_obj, "r", r_hex);
        yyjson_mut_obj_add_str(doc, sig_obj, "s", s_hex);
        yyjson_mut_obj_add_int(doc, sig_obj, "v", v);
    }
    yyjson_mut_obj_add_val(doc, root, "signature", sig_obj);

    if (vault_addr) {
        yyjson_mut_obj_add_str(doc, root, "vaultAddress", vault_addr);
    }

    *out_json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    return *out_json ? 0 : -1;
}

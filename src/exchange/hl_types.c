/*
 * Msgpack serialization for Hyperliquid actions.
 *
 * Uses msgpack-c library. Field ordering is CRITICAL for correct signatures.
 * The ordering here matches the Hyperliquid Python SDK exactly.
 */

#include "exchange/hl_types.h"
#include <msgpack.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Helper: pack a decimal as a string with trailing zeros stripped */
static void pack_decimal_str(msgpack_packer *pk, tb_decimal_t d) {
    char buf[64];
    tb_decimal_to_str(d, buf, sizeof(buf));
    size_t len = strlen(buf);
    msgpack_pack_str(pk, len);
    msgpack_pack_str_body(pk, buf, len);
}

/* Helper: pack a C string */
static void pack_str(msgpack_packer *pk, const char *s) {
    size_t len = strlen(s);
    msgpack_pack_str(pk, len);
    msgpack_pack_str_body(pk, s, len);
}

/* Convert TIF enum to string */
static const char *tif_to_str(tb_tif_t tif) {
    switch (tif) {
        case TB_TIF_GTC: return "Gtc";
        case TB_TIF_IOC: return "Ioc";
        case TB_TIF_ALO: return "Alo";
        default: return "Gtc";
    }
}

/* Convert grouping to string */
static const char *grouping_to_str(tb_grouping_t g) {
    switch (g) {
        case TB_GROUP_NA:          return "na";
        case TB_GROUP_NORMAL_TPSL: return "normalTpsl";
        case TB_GROUP_POS_TPSL:    return "positionTpsl";
        default: return "na";
    }
}

/*
 * Pack a single order in wire format.
 * Map with keys in exact order: a, b, p, s, r, t [, c]
 */
static void pack_order(msgpack_packer *pk, const tb_order_request_t *o) {
    bool has_cloid = o->cloid[0] != '\0';
    int n_fields = has_cloid ? 7 : 6;

    msgpack_pack_map(pk, n_fields);

    /* a: asset index */
    pack_str(pk, "a");
    msgpack_pack_uint32(pk, o->asset);

    /* b: is_buy */
    pack_str(pk, "b");
    if (o->side == TB_SIDE_BUY)
        msgpack_pack_true(pk);
    else
        msgpack_pack_false(pk);

    /* p: price string */
    pack_str(pk, "p");
    pack_decimal_str(pk, o->price);

    /* s: size string */
    pack_str(pk, "s");
    pack_decimal_str(pk, o->size);

    /* r: reduce_only */
    pack_str(pk, "r");
    if (o->reduce_only)
        msgpack_pack_true(pk);
    else
        msgpack_pack_false(pk);

    /* t: order type */
    pack_str(pk, "t");
    if (o->type == TB_ORDER_LIMIT) {
        /* { "limit": { "tif": "Gtc" } } */
        msgpack_pack_map(pk, 1);
        pack_str(pk, "limit");
        msgpack_pack_map(pk, 1);
        pack_str(pk, "tif");
        pack_str(pk, tif_to_str(o->tif));
    } else {
        /* { "trigger": { "isMarket": bool, "triggerPx": str, "tpsl": str } } */
        msgpack_pack_map(pk, 1);
        pack_str(pk, "trigger");
        msgpack_pack_map(pk, 3);
        pack_str(pk, "isMarket");
        if (o->is_market) msgpack_pack_true(pk);
        else msgpack_pack_false(pk);
        pack_str(pk, "triggerPx");
        pack_decimal_str(pk, o->trigger_px);
        pack_str(pk, "tpsl");
        pack_str(pk, o->tpsl == TB_TPSL_TP ? "tp" : "sl");
    }

    /* c: cloid (optional) */
    if (has_cloid) {
        pack_str(pk, "c");
        pack_str(pk, o->cloid);
    }
}

int hl_msgpack_order_action(
    const tb_order_request_t *orders,
    int n_orders,
    tb_grouping_t grouping,
    uint8_t **out_data,
    size_t *out_len
) {
    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    /* Action map: { "type": "order", "orders": [...], "grouping": "na" } */
    msgpack_pack_map(&pk, 3);

    pack_str(&pk, "type");
    pack_str(&pk, "order");

    pack_str(&pk, "orders");
    msgpack_pack_array(&pk, n_orders);
    for (int i = 0; i < n_orders; i++) {
        pack_order(&pk, &orders[i]);
    }

    pack_str(&pk, "grouping");
    pack_str(&pk, grouping_to_str(grouping));

    /* Copy result */
    *out_data = malloc(sbuf.size);
    if (!*out_data) {
        msgpack_sbuffer_destroy(&sbuf);
        return -1;
    }
    memcpy(*out_data, sbuf.data, sbuf.size);
    *out_len = sbuf.size;

    msgpack_sbuffer_destroy(&sbuf);
    return 0;
}

int hl_msgpack_cancel_action(
    const uint32_t *assets,
    const uint64_t *oids,
    int n_cancels,
    uint8_t **out_data,
    size_t *out_len
) {
    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    /* { "type": "cancel", "cancels": [{ "a": N, "o": N }, ...] } */
    msgpack_pack_map(&pk, 2);

    pack_str(&pk, "type");
    pack_str(&pk, "cancel");

    pack_str(&pk, "cancels");
    msgpack_pack_array(&pk, n_cancels);
    for (int i = 0; i < n_cancels; i++) {
        msgpack_pack_map(&pk, 2);
        pack_str(&pk, "a");
        msgpack_pack_uint32(&pk, assets[i]);
        pack_str(&pk, "o");
        msgpack_pack_uint64(&pk, oids[i]);
    }

    *out_data = malloc(sbuf.size);
    if (!*out_data) {
        msgpack_sbuffer_destroy(&sbuf);
        return -1;
    }
    memcpy(*out_data, sbuf.data, sbuf.size);
    *out_len = sbuf.size;

    msgpack_sbuffer_destroy(&sbuf);
    return 0;
}

int hl_msgpack_update_leverage_action(
    uint32_t asset,
    bool is_cross,
    int leverage,
    uint8_t **out_data,
    size_t *out_len
) {
    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    /* { "type": "updateLeverage", "asset": N, "isCross": bool, "leverage": N } */
    msgpack_pack_map(&pk, 4);

    pack_str(&pk, "type");
    pack_str(&pk, "updateLeverage");

    pack_str(&pk, "asset");
    msgpack_pack_uint32(&pk, asset);

    pack_str(&pk, "isCross");
    if (is_cross) msgpack_pack_true(&pk);
    else msgpack_pack_false(&pk);

    pack_str(&pk, "leverage");
    msgpack_pack_int(&pk, leverage);

    *out_data = malloc(sbuf.size);
    if (!*out_data) {
        msgpack_sbuffer_destroy(&sbuf);
        return -1;
    }
    memcpy(*out_data, sbuf.data, sbuf.size);
    *out_len = sbuf.size;

    msgpack_sbuffer_destroy(&sbuf);
    return 0;
}

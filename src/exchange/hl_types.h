#ifndef TB_HL_TYPES_H
#define TB_HL_TYPES_H

/*
 * Hyperliquid wire format serialization.
 *
 * CRITICAL: msgpack field ordering must exactly match the Python SDK.
 * Incorrect ordering produces valid msgpack but invalid signatures.
 *
 * Order action fields: type, orders[], grouping
 * Order fields: a, b, p, s, r, t, c (optional cloid)
 * Cancel action fields: type, cancels[]
 * Cancel fields: a, o
 */

#include "core/types.h"
#include <stdint.h>
#include <stddef.h>

/* Serialize an order action to msgpack for signing.
 * Caller must free *out_data. Returns 0 on success. */
int hl_msgpack_order_action(
    const tb_order_request_t *orders,
    int n_orders,
    tb_grouping_t grouping,
    uint8_t **out_data,
    size_t *out_len
);

/* Serialize a cancel action to msgpack for signing.
 * Caller must free *out_data. Returns 0 on success. */
int hl_msgpack_cancel_action(
    const uint32_t *assets,
    const uint64_t *oids,
    int n_cancels,
    uint8_t **out_data,
    size_t *out_len
);

/* Serialize an updateLeverage action to msgpack.
 * Caller must free *out_data. */
int hl_msgpack_update_leverage_action(
    uint32_t asset,
    bool is_cross,
    int leverage,
    uint8_t **out_data,
    size_t *out_len
);

#endif /* TB_HL_TYPES_H */

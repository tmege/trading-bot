#ifndef TB_AI_ADVISOR_H
#define TB_AI_ADVISOR_H

#include "core/config.h"
#include "core/types.h"
#include "data/macro_fetcher.h"
#include "data/twitter_sentiment.h"
#include "data/fear_greed.h"
#include "exchange/position_tracker.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct tb_lua_engine tb_lua_engine_t;

typedef struct tb_ai_advisor tb_ai_advisor_t;

/* ── Advisory context: everything the AI needs to make decisions ─────────── */
typedef struct {
    /* Current state */
    tb_macro_data_t      macro;
    tb_sentiment_data_t  sentiment;
    tb_fear_greed_t      fear_greed;

    /* Positions & P&L */
    tb_position_t        positions[TB_MAX_POSITIONS];
    int                  n_positions;
    double               account_value;
    double               daily_pnl;
    double               daily_fees;
    int                  daily_trades;

    /* Strategy parameters (current) */
    struct {
        char   name[64];
        bool   enabled;
    } strategies[8];
    int n_strategies;

    /* Recent trade history (last 24h) */
    struct {
        char   coin[16];
        char   side[8];
        double price;
        double size;
        double pnl;
        double fee;
        int64_t time_ms;
    } recent_trades[64];
    int n_recent_trades;
} tb_advisory_context_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

tb_ai_advisor_t *tb_ai_advisor_create(const tb_config_t *cfg, sqlite3 *db);
void             tb_ai_advisor_destroy(tb_ai_advisor_t *adv);

/* Set the Lua engine for dispatching advisory adjustments */
void tb_ai_advisor_set_lua_engine(tb_ai_advisor_t *adv, tb_lua_engine_t *engine);

/* Start background thread (checks time, calls at configured hours) */
int  tb_ai_advisor_start(tb_ai_advisor_t *adv);
void tb_ai_advisor_stop(tb_ai_advisor_t *adv);

/* Update the context that will be sent to the AI on next call */
void tb_ai_advisor_update_context(tb_ai_advisor_t *adv, const tb_advisory_context_t *ctx);

/* Force an immediate advisory call (for testing or manual trigger) */
int tb_ai_advisor_call_now(tb_ai_advisor_t *adv);

/* Get time of last advisory call */
int64_t tb_ai_advisor_last_call_time(const tb_ai_advisor_t *adv);

#endif /* TB_AI_ADVISOR_H */

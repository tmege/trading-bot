#include "core/config.h"
#include "core/logging.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Safe string copy from JSON value */
static void json_str(yyjson_val *obj, const char *key, char *dst, size_t dst_len,
                     const char *fallback) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    const char *s = yyjson_get_str(val);
    snprintf(dst, dst_len, "%s", s ? s : fallback);
}

static double json_num(yyjson_val *obj, const char *key, double fallback) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    return yyjson_is_num(val) ? yyjson_get_num(val) : fallback;
}

static int json_int(yyjson_val *obj, const char *key, int fallback) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    return yyjson_is_int(val) ? (int)yyjson_get_int(val) : fallback;
}

static bool json_bool(yyjson_val *obj, const char *key, bool fallback) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    return yyjson_is_bool(val) ? yyjson_get_bool(val) : fallback;
}

/* Load a secret from environment variable. Returns 0 on success. */
static int load_env_secret(const char *env_name, char *dst, size_t dst_len,
                           bool required) {
    const char *val = getenv(env_name);
    if (val && val[0]) {
        snprintf(dst, dst_len, "%s", val);
        return 0;
    }
    dst[0] = '\0';
    if (required) {
        fprintf(stderr, "ERROR: required env var %s not set\n", env_name);
        return -1;
    }
    return 0;
}

int tb_config_load(tb_config_t *cfg, const char *json_path) {
    memset(cfg, 0, sizeof(*cfg));

    /* Read and parse JSON */
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(json_path, 0, NULL, &err);
    if (!doc) {
        fprintf(stderr, "ERROR: failed to read config %s: %s\n",
                json_path, err.msg);
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);

    /* Exchange section */
    yyjson_val *exchange = yyjson_obj_get(root, "exchange");
    if (exchange) {
        json_str(exchange, "rest_url", cfg->rest_url, sizeof(cfg->rest_url),
                 "https://api.hyperliquid.xyz");
        json_str(exchange, "ws_url", cfg->ws_url, sizeof(cfg->ws_url),
                 "wss://api.hyperliquid.xyz/ws");
        cfg->is_testnet = json_bool(exchange, "is_testnet", false);
    } else {
        snprintf(cfg->rest_url, sizeof(cfg->rest_url),
                 "https://api.hyperliquid.xyz");
        snprintf(cfg->ws_url, sizeof(cfg->ws_url),
                 "wss://api.hyperliquid.xyz/ws");
    }

    /* Risk section */
    yyjson_val *risk = yyjson_obj_get(root, "risk");
    if (risk) {
        cfg->daily_loss_limit    = json_num(risk, "daily_loss_limit_usdc", -15.0);
        cfg->max_leverage        = json_num(risk, "max_leverage", 5.0);
        cfg->per_trade_stop_pct  = json_num(risk, "per_trade_stop_pct", 2.0);
        cfg->max_position_usd    = json_num(risk, "max_position_usd", 300.0);
        cfg->emergency_close_usd = json_num(risk, "emergency_close_usdc", -12.0);
    } else {
        cfg->daily_loss_limit    = -15.0;
        cfg->max_leverage        = 5.0;
        cfg->per_trade_stop_pct  = 2.0;
        cfg->max_position_usd    = 300.0;
        cfg->emergency_close_usd = -12.0;
    }

    /* Strategies section */
    yyjson_val *strat = yyjson_obj_get(root, "strategies");
    if (strat) {
        json_str(strat, "dir", cfg->strategies_dir, sizeof(cfg->strategies_dir),
                 "./strategies");
        cfg->strategy_reload_sec = json_int(strat, "reload_interval_sec", 5);

        /* Parse active strategies array */
        cfg->n_active_strategies = 0;
        yyjson_val *active = yyjson_obj_get(strat, "active");
        if (active && yyjson_is_arr(active)) {
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(active, idx, max, item) {
                if (cfg->n_active_strategies >= 8) break;
                int i = cfg->n_active_strategies;
                yyjson_val *f = yyjson_obj_get(item, "file");
                yyjson_val *r = yyjson_obj_get(item, "role");
                if (yyjson_get_str(f)) {
                    snprintf(cfg->active_strategies[i], 64, "%s", yyjson_get_str(f));
                    snprintf(cfg->strategy_roles[i], 16, "%s",
                             yyjson_get_str(r) ? yyjson_get_str(r) : "secondary");

                    /* Parse optional "coins" array */
                    cfg->n_strategy_coins[i] = 0;
                    yyjson_val *coins = yyjson_obj_get(item, "coins");
                    if (coins && yyjson_is_arr(coins)) {
                        size_t cidx, cmax;
                        yyjson_val *coin;
                        yyjson_arr_foreach(coins, cidx, cmax, coin) {
                            if (cfg->n_strategy_coins[i] >= TB_MAX_STRATEGY_COINS) break;
                            const char *cs = yyjson_get_str(coin);
                            if (cs) {
                                snprintf(cfg->strategy_coins[i][cfg->n_strategy_coins[i]],
                                         16, "%s", cs);
                                cfg->n_strategy_coins[i]++;
                            }
                        }
                    }

                    cfg->n_active_strategies++;
                }
            }
        }
    } else {
        snprintf(cfg->strategies_dir, sizeof(cfg->strategies_dir), "./strategies");
        cfg->strategy_reload_sec = 5;
        cfg->n_active_strategies = 0;
    }

    /* AI Advisory section */
    yyjson_val *ai = yyjson_obj_get(root, "ai_advisory");
    if (ai) {
        json_str(ai, "model", cfg->claude_model, sizeof(cfg->claude_model),
                 "claude-haiku-4-5-20251001");
        cfg->advisory_hour_morning = json_int(ai, "morning_hour_utc", 8);
        cfg->advisory_hour_evening = json_int(ai, "evening_hour_utc", 20);
    } else {
        snprintf(cfg->claude_model, sizeof(cfg->claude_model),
                 "claude-haiku-4-5-20251001");
        cfg->advisory_hour_morning = 8;
        cfg->advisory_hour_evening = 20;
    }

    /* Database section */
    yyjson_val *db = yyjson_obj_get(root, "database");
    if (db) {
        json_str(db, "path", cfg->db_path, sizeof(cfg->db_path),
                 "./data/trading_bot.db");
    } else {
        snprintf(cfg->db_path, sizeof(cfg->db_path), "./data/trading_bot.db");
    }

    /* Logging section */
    yyjson_val *logging = yyjson_obj_get(root, "logging");
    if (logging) {
        json_str(logging, "dir", cfg->log_dir, sizeof(cfg->log_dir), "./logs");
        cfg->log_level = json_int(logging, "level", 0);
    } else {
        snprintf(cfg->log_dir, sizeof(cfg->log_dir), "./logs");
        cfg->log_level = 0;
    }

    /* Mode section */
    yyjson_val *mode = yyjson_obj_get(root, "mode");
    if (mode) {
        cfg->paper_trading = json_bool(mode, "paper_trading", true);
        cfg->paper_initial_balance = json_num(mode, "paper_initial_balance", 100.0);
    } else {
        cfg->paper_trading = true;
        cfg->paper_initial_balance = 100.0;
    }

    yyjson_doc_free(doc);

    /* Load secrets from environment variables.
     * In paper trading mode, private key and wallet address are optional. */
    bool creds_required = !cfg->paper_trading;
    int rc = 0;
    rc |= load_env_secret("TB_PRIVATE_KEY", cfg->private_key_hex,
                           sizeof(cfg->private_key_hex), creds_required);
    rc |= load_env_secret("TB_WALLET_ADDRESS", cfg->wallet_address,
                           sizeof(cfg->wallet_address), creds_required);
    rc |= load_env_secret("TB_CLAUDE_API_KEY", cfg->claude_api_key,
                           sizeof(cfg->claude_api_key), false);
    rc |= load_env_secret("TB_MACRO_API_KEY", cfg->macro_api_key,
                           sizeof(cfg->macro_api_key), false);

    return rc;
}

void tb_config_dump(const tb_config_t *cfg) {
    tb_log_info("CONFIG: rest_url=%s", cfg->rest_url);
    tb_log_info("CONFIG: ws_url=%s", cfg->ws_url);
    tb_log_info("CONFIG: testnet=%s", cfg->is_testnet ? "true" : "false");
    if (cfg->wallet_address[0] && strlen(cfg->wallet_address) >= 6) {
        tb_log_info("CONFIG: wallet=%.6s...%s",
                    cfg->wallet_address,
                    cfg->wallet_address + strlen(cfg->wallet_address) - 4);
    } else {
        tb_log_info("CONFIG: wallet=(not set)");
    }
    tb_log_info("CONFIG: daily_loss_limit=%.2f USDC", cfg->daily_loss_limit);
    tb_log_info("CONFIG: emergency_close=%.2f USDC", cfg->emergency_close_usd);
    tb_log_info("CONFIG: max_leverage=%.1fx", cfg->max_leverage);
    tb_log_info("CONFIG: strategies_dir=%s", cfg->strategies_dir);
    for (int i = 0; i < cfg->n_active_strategies; i++) {
        if (cfg->n_strategy_coins[i] > 0) {
            char coins_buf[256] = "";
            for (int c = 0; c < cfg->n_strategy_coins[i]; c++) {
                if (c > 0) strncat(coins_buf, ",", sizeof(coins_buf) - strlen(coins_buf) - 1);
                strncat(coins_buf, cfg->strategy_coins[i][c],
                        sizeof(coins_buf) - strlen(coins_buf) - 1);
            }
            tb_log_info("CONFIG: strategy[%d]=%s (%s) coins=[%s]", i,
                        cfg->active_strategies[i], cfg->strategy_roles[i], coins_buf);
        } else {
            tb_log_info("CONFIG: strategy[%d]=%s (%s)", i,
                        cfg->active_strategies[i], cfg->strategy_roles[i]);
        }
    }
    tb_log_info("CONFIG: paper_trading=%s (balance=$%.0f)",
                cfg->paper_trading ? "true" : "false",
                cfg->paper_initial_balance);
    tb_log_info("CONFIG: db_path=%s", cfg->db_path);
    tb_log_info("CONFIG: claude_model=%s", cfg->claude_model);
    tb_log_info("CONFIG: claude_api_key=%s",
                cfg->claude_api_key[0] ? "***set***" : "(not set)");
}

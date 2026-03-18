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

/* Load a secret from environment variable and clear it from env. Returns 0 on success. */
static int load_env_secret(const char *env_name, char *dst, size_t dst_len,
                           bool required) {
    const char *val = getenv(env_name);
    if (val && val[0]) {
        snprintf(dst, dst_len, "%s", val);
        /* Clear from environment to reduce exposure to child processes */
        unsetenv(env_name);
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

    /* Risk section (percentage-based) */
    yyjson_val *risk = yyjson_obj_get(root, "risk");
    if (risk) {
        cfg->daily_loss_pct      = json_num(risk, "daily_loss_pct", 15.0);
        cfg->max_leverage        = json_num(risk, "max_leverage", 5.0);
        cfg->max_position_pct    = json_num(risk, "max_position_pct", 200.0);
        cfg->emergency_close_pct = json_num(risk, "emergency_close_pct", 12.0);
    } else {
        cfg->daily_loss_pct      = 15.0;
        cfg->max_leverage        = 5.0;
        cfg->max_position_pct    = 200.0;
        cfg->emergency_close_pct = 12.0;
    }

    /* Validate risk parameters — prevent misconfigured safety bypass */
    if (cfg->daily_loss_pct <= 0 || cfg->daily_loss_pct > 50.0) {
        fprintf(stderr, "WARN: daily_loss_pct=%.1f out of range (0,50], clamping to 15\n",
                cfg->daily_loss_pct);
        cfg->daily_loss_pct = 15.0;
    }
    if (cfg->emergency_close_pct <= 0 || cfg->emergency_close_pct > cfg->daily_loss_pct) {
        fprintf(stderr, "WARN: emergency_close_pct=%.1f invalid (must be in (0,daily_loss_pct]), clamping to %.1f\n",
                cfg->emergency_close_pct, cfg->daily_loss_pct * 0.8);
        cfg->emergency_close_pct = cfg->daily_loss_pct * 0.8;
    }
    if (cfg->max_leverage <= 0 || cfg->max_leverage > 50.0) {
        fprintf(stderr, "WARN: max_leverage=%.1f out of range (0,50], clamping to 5\n",
                cfg->max_leverage);
        cfg->max_leverage = 5.0;
    }
    if (cfg->max_position_pct <= 0 || cfg->max_position_pct > 1000.0) {
        fprintf(stderr, "WARN: max_position_pct=%.0f out of range (0,1000], clamping to 200\n",
                cfg->max_position_pct);
        cfg->max_position_pct = 200.0;
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

                    /* Parse optional per-strategy paper_mode and paper_balance */
                    yyjson_val *pm = yyjson_obj_get(item, "paper_mode");
                    if (yyjson_is_bool(pm)) {
                        cfg->strategy_paper_set[i] = true;
                        cfg->strategy_paper_mode[i] = yyjson_get_bool(pm);
                    }
                    cfg->strategy_paper_balance[i] = json_num(item, "paper_balance", 0);

                    cfg->n_active_strategies++;
                }
            }
        }
    } else {
        snprintf(cfg->strategies_dir, sizeof(cfg->strategies_dir), "./strategies");
        cfg->strategy_reload_sec = 5;
        cfg->n_active_strategies = 0;
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
        int lvl = json_int(logging, "level", 0);
        cfg->log_level = (lvl >= 0 && lvl <= 4) ? lvl : 0;
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
     * Credentials required if any strategy runs in live mode. */
    bool any_live = false;
    for (int i = 0; i < cfg->n_active_strategies; i++) {
        bool is_paper = cfg->strategy_paper_set[i]
            ? cfg->strategy_paper_mode[i]
            : cfg->paper_trading;
        if (!is_paper) { any_live = true; break; }
    }
    /* If no active strategies, fall back to global flag */
    if (cfg->n_active_strategies == 0) any_live = !cfg->paper_trading;
    bool creds_required = any_live;
    int rc = 0;
    rc |= load_env_secret("TB_PRIVATE_KEY", cfg->private_key_hex,
                           sizeof(cfg->private_key_hex), creds_required);
    rc |= load_env_secret("TB_WALLET_ADDRESS", cfg->wallet_address,
                           sizeof(cfg->wallet_address), creds_required);
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
    tb_log_info("CONFIG: daily_loss_pct=%.1f%%", cfg->daily_loss_pct);
    tb_log_info("CONFIG: emergency_close_pct=%.1f%%", cfg->emergency_close_pct);
    tb_log_info("CONFIG: max_position_pct=%.0f%%", cfg->max_position_pct);
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
            if (cfg->strategy_paper_set[i]) {
                tb_log_info("CONFIG: strategy[%d]=%s (%s) coins=[%s] paper=%s%s", i,
                            cfg->active_strategies[i], cfg->strategy_roles[i], coins_buf,
                            cfg->strategy_paper_mode[i] ? "true" : "false",
                            cfg->strategy_paper_balance[i] > 0 ? "" : "");
                if (cfg->strategy_paper_balance[i] > 0) {
                    tb_log_info("CONFIG:   paper_balance=$%.0f", cfg->strategy_paper_balance[i]);
                }
            } else {
                tb_log_info("CONFIG: strategy[%d]=%s (%s) coins=[%s]", i,
                            cfg->active_strategies[i], cfg->strategy_roles[i], coins_buf);
            }
        } else {
            tb_log_info("CONFIG: strategy[%d]=%s (%s)", i,
                        cfg->active_strategies[i], cfg->strategy_roles[i]);
        }
    }
    tb_log_info("CONFIG: paper_trading=%s (balance=$%.0f)",
                cfg->paper_trading ? "true" : "false",
                cfg->paper_initial_balance);
    tb_log_info("CONFIG: db_path=%s", cfg->db_path);
}

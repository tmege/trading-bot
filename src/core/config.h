#ifndef TB_CONFIG_H
#define TB_CONFIG_H

#include <stdbool.h>

typedef struct {
    /* Exchange */
    char     rest_url[256];
    char     ws_url[256];
    bool     is_testnet;
    char     private_key_hex[70];     /* from env TB_PRIVATE_KEY (with optional 0x prefix) */
    char     wallet_address[44];      /* from env TB_WALLET_ADDRESS */

    /* Risk */
    double   daily_loss_limit;        /* e.g. -5.0 (USDC) */
    double   max_leverage;            /* e.g. 3.0 */
    double   per_trade_stop_pct;      /* e.g. 2.0 */
    double   max_position_usd;        /* e.g. 300.0 */
    double   emergency_close_usd;    /* e.g. -12.0 — close all before daily limit */

    /* Strategy */
    char     strategies_dir[512];
    int      strategy_reload_sec;
    char     active_strategies[8][64];   /* filenames of active strategies */
    char     strategy_roles[8][16];      /* "primary" or "secondary" */
    int      n_active_strategies;        /* how many active strategies */

    /* Per-strategy coins (multi-coin: 1 file → N instances) */
    #define TB_MAX_STRATEGY_COINS 16
    char     strategy_coins[8][TB_MAX_STRATEGY_COINS][16];
    int      n_strategy_coins[8];

    /* AI Advisory */
    char     claude_api_key[256];     /* from env TB_CLAUDE_API_KEY */
    char     claude_model[64];
    int      advisory_hour_morning;   /* UTC */
    int      advisory_hour_evening;   /* UTC */

    /* Data sources */
    char     macro_api_key[256];      /* from env TB_MACRO_API_KEY */

    /* Database */
    char     db_path[512];

    /* Logging */
    char     log_dir[512];
    int      log_level;               /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */

    /* Mode */
    bool     paper_trading;
} tb_config_t;

/* Load config from JSON file. Secrets come from environment variables. */
int  tb_config_load(tb_config_t *cfg, const char *json_path);

/* Log config with secrets redacted */
void tb_config_dump(const tb_config_t *cfg);

#endif /* TB_CONFIG_H */

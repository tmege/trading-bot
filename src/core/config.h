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

    /* Risk (percentage-based — scales with account value) */
    double   daily_loss_pct;          /* e.g. 15.0 → max 15% daily loss */
    double   emergency_close_pct;     /* e.g. 12.0 → emergency close at 12% loss */
    double   max_leverage;            /* e.g. 5.0 */
    double   max_position_pct;        /* e.g. 200.0 → max 200% of account per pos */

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

    /* Data sources */
    char     macro_api_key[256];      /* from env TB_MACRO_API_KEY */

    /* Database */
    char     db_path[512];

    /* Logging */
    char     log_dir[512];
    int      log_level;               /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */

    /* Mode */
    bool     paper_trading;
    double   paper_initial_balance;   /* starting bankroll for paper mode */
} tb_config_t;

/* Load config from JSON file. Secrets come from environment variables. */
int  tb_config_load(tb_config_t *cfg, const char *json_path);

/* Log config with secrets redacted */
void tb_config_dump(const tb_config_t *cfg);

#endif /* TB_CONFIG_H */

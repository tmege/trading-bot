#include "core/engine.h"
#include "core/config.h"
#include "core/logging.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void print_banner(const tb_config_t *cfg) {
    const char *mode = cfg->paper_trading ? "PAPER" : "LIVE";
    const char *color = cfg->paper_trading ? "\033[33m" : "\033[32m";

    /* Build dynamic coin list from config */
    char coins_str[256] = "";
    for (int i = 0; i < cfg->n_active_strategies; i++) {
        for (int c = 0; c < cfg->n_strategy_coins[i]; c++) {
            if (coins_str[0] != '\0')
                strncat(coins_str, "|", sizeof(coins_str) - strlen(coins_str) - 1);
            strncat(coins_str, cfg->strategy_coins[i][c],
                    sizeof(coins_str) - strlen(coins_str) - 1);
        }
    }
    if (coins_str[0] == '\0') snprintf(coins_str, sizeof(coins_str), "no coins configured");

    printf("\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║     TRADING BOT v0.5.0                   ║\n");
    printf("  ║     BB Scalping Multi-Coin — Hyperliquid ║\n");
    printf("  ║     %-37s║\n", coins_str);
    printf("  ║     Mode: %s%-5s\033[0m                          ║\n", color, mode);
    printf("  ╚══════════════════════════════════════════╝\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    const char *config_path = "config/bot_config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    /* Load config */
    tb_config_t cfg;
    if (tb_config_load(&cfg, config_path) != 0) {
        fprintf(stderr, "Failed to load config from %s\n", config_path);
        return 1;
    }

    print_banner(&cfg);

    /* Initialize logging */
    if (tb_log_init(cfg.log_dir, cfg.log_level) != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return 1;
    }

    /* Install signal handlers */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create and start engine */
    tb_engine_t *engine = tb_engine_create(&cfg);
    if (!engine) {
        tb_log_fatal("failed to create engine");
        tb_log_shutdown();
        return 1;
    }

    if (tb_engine_start(engine) != 0) {
        tb_log_fatal("failed to start engine");
        tb_engine_destroy(engine);
        tb_log_shutdown();
        return 1;
    }

    tb_log_info("bot running. Press Ctrl+C to stop.");

    /* Main loop */
    while (!g_shutdown && tb_engine_is_running(engine)) {
        usleep(100000); /* 100ms */
    }

    /* Graceful shutdown */
    tb_log_info("shutdown requested...");
    tb_engine_stop(engine);
    tb_engine_destroy(engine);

    /* Secure wipe of stack config (contains secrets) */
    volatile unsigned char *p = (volatile unsigned char *)&cfg;
    for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0;

    tb_log_shutdown();

    printf("\nBot stopped.\n");
    return 0;
}

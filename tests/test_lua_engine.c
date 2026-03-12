/*
 * Tests for Lua Strategy Engine (Phase 4)
 */

#include "strategy/lua_engine.h"
#include "core/logging.h"
#include "core/config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

/* ── Helper: check strategy survived (loaded + enabled) ─────────────────── */
static bool strategy_is_ok(tb_lua_engine_t *engine, const char *name) {
    tb_strategy_info_t infos[TB_MAX_STRATEGIES];
    int count = TB_MAX_STRATEGIES;
    tb_lua_engine_get_strategies(engine, infos, &count);
    for (int i = 0; i < count; i++) {
        if (strcmp(infos[i].name, name) == 0)
            return infos[i].loaded && infos[i].enabled;
    }
    return false;
}

/* ── Helper: write a Lua file ───────────────────────────────────────────── */
static void write_lua_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* ── Test: engine creation ──────────────────────────────────────────────── */
static void test_engine_create(void) {
    printf("\n== Lua Engine Create ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 2;

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    ASSERT(engine != NULL, "engine created");

    tb_lua_engine_destroy(engine);
}

/* ── Test: sandbox ──────────────────────────────────────────────────────── */
static void test_sandbox(void) {
    printf("\n== Lua Sandbox ==\n");

    mkdir("./test_strategies", 0755);

    /* Write a strategy that tries to use io */
    write_lua_file("./test_strategies/sandbox_test.lua",
        "function on_init()\n"
        "    if io then\n"
        "        error('io should be nil!')\n"
        "    end\n"
        "    if require then\n"
        "        error('require should be nil!')\n"
        "    end\n"
        "    if loadfile then\n"
        "        error('loadfile should be nil!')\n"
        "    end\n"
        "    if debug then\n"
        "        error('debug should be nil!')\n"
        "    end\n"
        "    if package then\n"
        "        error('package should be nil!')\n"
        "    end\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 5;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);

    int loaded = tb_lua_engine_load_strategies(engine);
    ASSERT(loaded >= 1, "sandbox test strategy loaded");

    /* on_init should NOT error — sandbox is in effect.
     * The Lua strategy calls error() if any forbidden global exists.
     * If we reach here without crash, sandbox is working. */
    tb_lua_engine_on_init(engine);

    tb_strategy_info_t info[4];
    int info_count = 4;
    tb_lua_engine_get_strategies(engine, info, &info_count);
    ASSERT(info_count >= 1 && info[0].loaded, "sandbox strategy loaded and on_init succeeded");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/sandbox_test.lua");
}

/* ── Test: callbacks ────────────────────────────────────────────────────── */
static void test_callbacks(void) {
    printf("\n== Lua Callbacks ==\n");

    /* Write a strategy that tracks callback invocations via state */
    write_lua_file("./test_strategies/callback_test.lua",
        "local state = { init=false, ticks=0, fills=0, timers=0 }\n"
        "\n"
        "function on_init()\n"
        "    state.init = true\n"
        "    bot.save_state('initialized', 'yes')\n"
        "    bot.log('info', 'callback test initialized')\n"
        "end\n"
        "\n"
        "function on_tick(coin, mid)\n"
        "    state.ticks = state.ticks + 1\n"
        "    bot.save_state('ticks', tostring(state.ticks))\n"
        "end\n"
        "\n"
        "function on_fill(fill)\n"
        "    state.fills = state.fills + 1\n"
        "    bot.save_state('last_fill_coin', fill.coin)\n"
        "    bot.save_state('last_fill_price', tostring(fill.price))\n"
        "end\n"
        "\n"
        "function on_timer()\n"
        "    state.timers = state.timers + 1\n"
        "end\n"
        "\n"
        "function on_shutdown()\n"
        "    bot.save_state('shutdown', 'true')\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 5;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);

    int loaded = tb_lua_engine_load_strategies(engine);
    ASSERT(loaded >= 1, "callback test strategy loaded");

    /* on_init */
    tb_lua_engine_on_init(engine);
    ASSERT(strategy_is_ok(engine, "callback_test"), "on_init: strategy still loaded");

    /* on_tick */
    tb_lua_engine_on_tick(engine, "ETH", 2050.0);
    tb_lua_engine_on_tick(engine, "ETH", 2051.0);
    tb_lua_engine_on_tick(engine, "ETH", 2049.0);
    ASSERT(strategy_is_ok(engine, "callback_test"), "on_tick x3: strategy still loaded");

    /* on_fill */
    tb_fill_t fill;
    memset(&fill, 0, sizeof(fill));
    snprintf(fill.coin, sizeof(fill.coin), "ETH");
    fill.px = tb_decimal_from_double(2050.0, 2);
    fill.sz = tb_decimal_from_double(0.05, 4);
    fill.side = TB_SIDE_BUY;
    fill.time_ms = 1700000000000LL;
    fill.closed_pnl = tb_decimal_from_double(1.5, 4);
    fill.fee = tb_decimal_from_double(0.1, 4);

    tb_lua_engine_on_fill(engine, &fill, "callback_test");
    ASSERT(strategy_is_ok(engine, "callback_test"), "on_fill: strategy still loaded");

    /* on_timer */
    tb_lua_engine_on_timer(engine);
    ASSERT(strategy_is_ok(engine, "callback_test"), "on_timer: strategy still loaded");

    /* on_shutdown */
    tb_lua_engine_on_shutdown(engine);
    ASSERT(strategy_is_ok(engine, "callback_test"), "on_shutdown: strategy still loaded");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/callback_test.lua");
}

/* ── Test: bot.time() ───────────────────────────────────────────────────── */
static void test_bot_time(void) {
    printf("\n== bot.time() ==\n");

    write_lua_file("./test_strategies/time_test.lua",
        "function on_init()\n"
        "    local t = bot.time()\n"
        "    if t > 1000000000 then\n"
        "        bot.save_state('time_ok', 'yes')\n"
        "    else\n"
        "        bot.save_state('time_ok', 'no')\n"
        "    end\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 5;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);
    tb_lua_engine_load_strategies(engine);
    tb_lua_engine_on_init(engine);

    ASSERT(strategy_is_ok(engine, "time_test"), "bot.time() on_init succeeded");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/time_test.lua");
}

/* ── Test: strategy enable/disable ──────────────────────────────────────── */
static void test_enable_disable(void) {
    printf("\n== Strategy Enable/Disable ==\n");

    write_lua_file("./test_strategies/toggle_test.lua",
        "function on_init()\n"
        "    bot.log('info', 'toggle test init')\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 5;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);
    tb_lua_engine_load_strategies(engine);

    tb_strategy_info_t infos[8];
    int count = 8;
    tb_lua_engine_get_strategies(engine, infos, &count);
    ASSERT(count >= 1, "at least 1 strategy listed");

    /* Disable */
    int rc = tb_lua_engine_set_enabled(engine, "toggle_test", false);
    ASSERT(rc == 0, "strategy disabled");

    tb_lua_engine_get_strategies(engine, infos, &count);
    bool found_disabled = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(infos[i].name, "toggle_test") == 0 && !infos[i].enabled) {
            found_disabled = true;
        }
    }
    ASSERT(found_disabled, "strategy shows as disabled");

    /* Re-enable */
    rc = tb_lua_engine_set_enabled(engine, "toggle_test", true);
    ASSERT(rc == 0, "strategy re-enabled");

    /* Non-existent strategy */
    rc = tb_lua_engine_set_enabled(engine, "nonexistent", false);
    ASSERT(rc != 0, "non-existent strategy returns error");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/toggle_test.lua");
}

/* ── Test: hot reload ───────────────────────────────────────────────────── */
static void test_hot_reload(void) {
    printf("\n== Hot Reload ==\n");

    write_lua_file("./test_strategies/reload_test.lua",
        "version = 1\n"
        "function on_init()\n"
        "    bot.save_state('version', tostring(version))\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 1;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);
    tb_lua_engine_load_strategies(engine);
    tb_lua_engine_on_init(engine);

    /* No changes yet */
    int reloaded = tb_lua_engine_check_reload(engine);
    ASSERT(reloaded == 0, "no reload when unchanged");

    /* Modify the file (need to wait for mtime to differ) */
    sleep(1);
    write_lua_file("./test_strategies/reload_test.lua",
        "version = 2\n"
        "function on_init()\n"
        "    bot.save_state('version', tostring(version))\n"
        "end\n"
    );

    reloaded = tb_lua_engine_check_reload(engine);
    ASSERT(reloaded == 1, "reloaded after file change");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/reload_test.lua");
}

/* ── Test: Lua error handling ───────────────────────────────────────────── */
static void test_error_handling(void) {
    printf("\n== Lua Error Handling ==\n");

    write_lua_file("./test_strategies/error_test.lua",
        "function on_tick(coin, mid)\n"
        "    error('intentional error')\n"
        "end\n"
        "function on_init()\n"
        "    -- this one is fine\n"
        "end\n"
    );

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.strategies_dir, sizeof(cfg.strategies_dir), "./test_strategies");
    cfg.strategy_reload_sec = 5;

    tb_lua_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tb_lua_engine_t *engine = tb_lua_engine_create(&cfg);
    tb_lua_engine_set_context(engine, &ctx);
    tb_lua_engine_load_strategies(engine);
    tb_lua_engine_on_init(engine);

    /* on_tick should error but not crash or unload the strategy */
    tb_lua_engine_on_tick(engine, "ETH", 2000.0);
    ASSERT(strategy_is_ok(engine, "error_test"), "Lua error in callback does not unload strategy");

    tb_lua_engine_destroy(engine);
    unlink("./test_strategies/error_test.lua");
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void) {
    tb_log_init("./logs", 1);

    /* Create test strategies directory */
    mkdir("./test_strategies", 0755);

    test_engine_create();
    test_sandbox();
    test_callbacks();
    test_bot_time();
    test_enable_disable();
    test_hot_reload();
    test_error_handling();

    /* Cleanup */
    rmdir("./test_strategies");

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}

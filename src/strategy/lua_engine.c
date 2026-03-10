#include "strategy/lua_engine.h"
#include "strategy/strategy_api.h"
#include "core/logging.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

/* ── Memory tracker (forward decl for slot struct) ─────────────────────── */
typedef struct {
    size_t used;
    size_t limit;
} lua_mem_tracker_t;

/* ── Per-strategy slot ──────────────────────────────────────────────────── */
typedef struct {
    char        name[64];
    char        path[512];
    char        coin[16];       /* injected COIN global (empty = no injection) */
    lua_State  *L;
    lua_mem_tracker_t *mem_tracker;  /* memory limit tracker */
    bool        loaded;
    bool        enabled;
    int64_t     last_mtime;
    tb_lua_ctx_t ctx;     /* strategy-specific context (shares pointers, own name) */
} tb_strategy_slot_t;

struct tb_lua_engine {
    tb_strategy_slot_t  slots[TB_MAX_STRATEGIES];
    int                 n_strategies;
    char                strategies_dir[512];
    int                 reload_sec;
    tb_lua_ctx_t       *shared_ctx;     /* shared context template */
    pthread_mutex_t     lock;
};

/* ── Lua execution limits ──────────────────────────────────────────────── */

/* Max instructions per Lua call (prevents infinite loops) */
#define LUA_MAX_INSTRUCTIONS 10000000

/* Max memory per Lua state (16 MB) */
#define LUA_MAX_MEMORY (16 * 1024 * 1024)

static void lua_instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "instruction limit exceeded (%d instructions)", LUA_MAX_INSTRUCTIONS);
}

static void *lua_mem_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    lua_mem_tracker_t *tracker = (lua_mem_tracker_t *)ud;
    if (nsize == 0) {
        /* Free */
        tracker->used -= osize;
        free(ptr);
        return NULL;
    }
    if (nsize > osize && (tracker->used + nsize - osize) > tracker->limit) {
        /* Allocation would exceed limit */
        return NULL;
    }
    void *new_ptr = realloc(ptr, nsize);
    if (new_ptr) {
        tracker->used += nsize - osize;
    }
    return new_ptr;
}

/* ── Sandbox: remove dangerous functions ────────────────────────────────── */
static void sandbox_lua_state(lua_State *L) {
    /* Remove os (except os.clock, os.time, os.difftime) */
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        /* Keep only safe functions */
        lua_getfield(L, -1, "clock");
        lua_getfield(L, -2, "time");
        lua_getfield(L, -3, "difftime");

        /* Clear the table */
        lua_newtable(L);
        lua_pushvalue(L, -4);  /* clock */
        lua_setfield(L, -2, "clock");
        lua_pushvalue(L, -4);  /* time */
        lua_setfield(L, -2, "time");
        lua_pushvalue(L, -4);  /* difftime */
        lua_setfield(L, -2, "difftime");
        lua_setglobal(L, "os");
        lua_pop(L, 4); /* pop old os + 3 functions */
    } else {
        lua_pop(L, 1);
    }

    /* Remove io entirely */
    lua_pushnil(L);
    lua_setglobal(L, "io");

    /* Remove loadfile, dofile, load with file access */
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    lua_pushnil(L);
    lua_setglobal(L, "dofile");

    /* Remove package/require (no external module loading) */
    lua_pushnil(L);
    lua_setglobal(L, "require");

    lua_pushnil(L);
    lua_setglobal(L, "package");

    /* Remove debug library */
    lua_pushnil(L);
    lua_setglobal(L, "debug");

    /* Remove load() — can compile arbitrary code from strings */
    lua_pushnil(L);
    lua_setglobal(L, "load");

    /* Remove loadstring (Lua 5.1 compat — alias for load()) */
    lua_pushnil(L);
    lua_setglobal(L, "loadstring");

    /* Remove rawget/rawset/metatable manipulation for sandbox integrity */
    lua_pushnil(L);
    lua_setglobal(L, "rawget");
    lua_pushnil(L);
    lua_setglobal(L, "rawset");
    lua_pushnil(L);
    lua_setglobal(L, "setmetatable");
    lua_pushnil(L);
    lua_setglobal(L, "getmetatable");

    /* Remove collectgarbage (info leak + DoS) */
    lua_pushnil(L);
    lua_setglobal(L, "collectgarbage");

    /* Remove string.dump (bytecode serialization — sandbox escape vector) */
    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
    }
    lua_pop(L, 1);
}

/* ── Get file mtime ─────────────────────────────────────────────────────── */
static int64_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

/* ── Load a single strategy from file ───────────────────────────────────── */
static int load_strategy_file(tb_lua_engine_t *engine, tb_strategy_slot_t *slot) {
    /* Create new Lua state with memory limit */
    lua_mem_tracker_t *tracker = calloc(1, sizeof(lua_mem_tracker_t));
    if (!tracker) {
        tb_log_error("lua: failed to allocate memory tracker for %s", slot->name);
        return -1;
    }
    tracker->limit = LUA_MAX_MEMORY;

    lua_State *L = lua_newstate(lua_mem_alloc, tracker, 0);
    if (!L) {
        free(tracker);
        tb_log_error("lua: failed to create state for %s", slot->name);
        return -1;
    }

    /* Open standard libs, then sandbox */
    luaL_openlibs(L);
    sandbox_lua_state(L);

    /* Set instruction count limit to prevent infinite loops */
    lua_sethook(L, lua_instruction_hook, LUA_MASKCOUNT, LUA_MAX_INSTRUCTIONS);

    /* Set up strategy-specific context */
    slot->ctx = *engine->shared_ctx;
    slot->ctx.strategy_name = slot->name;

    /* Register bot.* API */
    tb_strategy_api_register(L, &slot->ctx);

    /* Inject COIN global if this slot has a coin assigned */
    if (slot->coin[0] != '\0') {
        lua_pushstring(L, slot->coin);
        lua_setglobal(L, "COIN");
    }

    /* Load and execute the file */
    if (luaL_dofile(L, slot->path) != LUA_OK) {
        tb_log_error("lua: error loading %s: %s", slot->name, lua_tostring(L, -1));
        lua_close(L);
        free(tracker);
        return -1;
    }

    /* Close old state if reloading */
    if (slot->L) {
        /* Call on_shutdown on old state first */
        lua_getglobal(slot->L, "on_shutdown");
        if (lua_isfunction(slot->L, -1)) {
            if (lua_pcall(slot->L, 0, 0, 0) != LUA_OK) {
                tb_log_warn("lua: %s on_shutdown error: %s",
                           slot->name, lua_tostring(slot->L, -1));
                lua_pop(slot->L, 1);
            }
        } else {
            lua_pop(slot->L, 1);
        }
        lua_close(slot->L);
        free(slot->mem_tracker);
    }

    slot->L = L;
    slot->mem_tracker = tracker;
    slot->loaded = true;
    slot->enabled = true;
    slot->last_mtime = file_mtime(slot->path);

    tb_log_info("lua: loaded strategy '%s' from %s", slot->name, slot->path);
    return 0;
}

/* ── Extract strategy name from filename ────────────────────────────────── */
static void name_from_path(const char *path, char *name, size_t name_len) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;

    snprintf(name, name_len, "%s", base);

    /* Remove .lua extension */
    char *dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".lua") == 0) {
        *dot = '\0';
    }
}

/* ── Engine lifecycle ───────────────────────────────────────────────────── */

tb_lua_engine_t *tb_lua_engine_create(const tb_config_t *cfg) {
    tb_lua_engine_t *engine = calloc(1, sizeof(tb_lua_engine_t));
    if (!engine) return NULL;

    snprintf(engine->strategies_dir, sizeof(engine->strategies_dir),
             "%s", cfg->strategies_dir);
    engine->reload_sec = cfg->strategy_reload_sec > 0 ? cfg->strategy_reload_sec : 5;
    /* Recursive mutex: on_tick → place_limit → immediate fill → on_fill
     * can re-enter from the same thread. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&engine->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    tb_log_info("lua engine: dir=%s, reload=%ds",
                engine->strategies_dir, engine->reload_sec);
    return engine;
}

void tb_lua_engine_destroy(tb_lua_engine_t *engine) {
    if (!engine) return;

    for (int i = 0; i < engine->n_strategies; i++) {
        if (engine->slots[i].L) {
            lua_close(engine->slots[i].L);
            engine->slots[i].L = NULL;
        }
        free(engine->slots[i].mem_tracker);
        engine->slots[i].mem_tracker = NULL;
    }

    pthread_mutex_destroy(&engine->lock);
    free(engine);
}

void tb_lua_engine_set_context(tb_lua_engine_t *engine, tb_lua_ctx_t *ctx) {
    engine->shared_ctx = ctx;

    /* Update context in already-loaded strategies */
    for (int i = 0; i < engine->n_strategies; i++) {
        if (engine->slots[i].loaded && engine->slots[i].L) {
            engine->slots[i].ctx = *ctx;
            engine->slots[i].ctx.strategy_name = engine->slots[i].name;
            tb_strategy_api_set_context(engine->slots[i].L, &engine->slots[i].ctx);
        }
    }
}

int tb_lua_engine_load_strategies(tb_lua_engine_t *engine) {
    pthread_mutex_lock(&engine->lock);

    const tb_config_t *cfg = engine->shared_ctx ? engine->shared_ctx->config : NULL;
    int loaded = 0;

    if (cfg && cfg->n_active_strategies > 0) {
        /* Load only active strategies from config */
        for (int i = 0; i < cfg->n_active_strategies && engine->n_strategies < TB_MAX_STRATEGIES; i++) {
            /* Path traversal protection: reject filenames with path separators */
            const char *fname = cfg->active_strategies[i];
            if (strchr(fname, '/') || strchr(fname, '\\') || strstr(fname, "..")) {
                tb_log_error("lua: REJECTED strategy filename '%s' (path traversal attempt)", fname);
                continue;
            }

            if (cfg->n_strategy_coins[i] > 0) {
                /* Multi-coin mode: create one slot per coin from the same file */
                char base_name[64];
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s",
                         engine->strategies_dir, fname);
                name_from_path(full_path, base_name, sizeof(base_name));

                for (int c = 0; c < cfg->n_strategy_coins[i] && engine->n_strategies < TB_MAX_STRATEGIES; c++) {
                    tb_strategy_slot_t *slot = &engine->slots[engine->n_strategies];
                    snprintf(slot->path, sizeof(slot->path), "%s", full_path);
                    snprintf(slot->coin, sizeof(slot->coin), "%s", cfg->strategy_coins[i][c]);

                    /* Instance name = base_coinlower, e.g. "bb_scalp_15m_eth" */
                    char coin_lower[16];
                    snprintf(coin_lower, sizeof(coin_lower), "%s", slot->coin);
                    for (char *p = coin_lower; *p; p++) {
                        if (*p >= 'A' && *p <= 'Z') *p += 32;
                    }
                    snprintf(slot->name, sizeof(slot->name), "%s_%s", base_name, coin_lower);

                    if (load_strategy_file(engine, slot) == 0) {
                        engine->n_strategies++;
                        loaded++;
                    } else {
                        tb_log_error("lua: failed to load '%s' for coin %s",
                                     fname, slot->coin);
                    }
                }
            } else {
                /* Legacy mode: single strategy, no coins array */
                tb_strategy_slot_t *slot = &engine->slots[engine->n_strategies];
                snprintf(slot->path, sizeof(slot->path), "%s/%s",
                         engine->strategies_dir, fname);
                name_from_path(slot->path, slot->name, sizeof(slot->name));
                slot->coin[0] = '\0';

                if (load_strategy_file(engine, slot) == 0) {
                    engine->n_strategies++;
                    loaded++;
                } else {
                    tb_log_error("lua: failed to load active strategy '%s'",
                                 cfg->active_strategies[i]);
                }
            }
        }
    } else {
        /* Fallback: load all .lua files from directory */
        DIR *dir = opendir(engine->strategies_dir);
        if (!dir) {
            tb_log_warn("lua: cannot open strategies dir: %s", engine->strategies_dir);
            pthread_mutex_unlock(&engine->lock);
            return -1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && engine->n_strategies < TB_MAX_STRATEGIES) {
            size_t len = strlen(entry->d_name);
            if (len < 5 || strcmp(entry->d_name + len - 4, ".lua") != 0)
                continue;
            if (strstr(entry->d_name, "template") != NULL)
                continue;

            tb_strategy_slot_t *slot = &engine->slots[engine->n_strategies];
            snprintf(slot->path, sizeof(slot->path), "%s/%s",
                     engine->strategies_dir, entry->d_name);
            name_from_path(slot->path, slot->name, sizeof(slot->name));

            if (load_strategy_file(engine, slot) == 0) {
                engine->n_strategies++;
                loaded++;
            }
        }

        closedir(dir);
    }

    pthread_mutex_unlock(&engine->lock);

    tb_log_info("lua: loaded %d strategies", loaded);
    return loaded;
}

int tb_lua_engine_check_reload(tb_lua_engine_t *engine) {
    pthread_mutex_lock(&engine->lock);
    int reloaded = 0;

    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        int64_t current_mtime = file_mtime(slot->path);

        if (current_mtime > slot->last_mtime) {
            tb_log_info("lua: detected change in %s, reloading...", slot->name);
            if (load_strategy_file(engine, slot) == 0) {
                reloaded++;
                /* Call on_init on the new state */
                lua_getglobal(slot->L, "on_init");
                if (lua_isfunction(slot->L, -1)) {
                    if (lua_pcall(slot->L, 0, 0, 0) != LUA_OK) {
                        tb_log_error("lua: %s on_init error after reload: %s",
                                    slot->name, lua_tostring(slot->L, -1));
                        lua_pop(slot->L, 1);
                    }
                } else {
                    lua_pop(slot->L, 1);
                }
            }
        }
    }

    pthread_mutex_unlock(&engine->lock);
    return reloaded;
}

/* ── Callback helpers ───────────────────────────────────────────────────── */

#define CALL_LUA_VOID(slot, func_name) do { \
    if (!(slot)->loaded || !(slot)->enabled || !(slot)->L) continue; \
    lua_getglobal((slot)->L, func_name); \
    if (!lua_isfunction((slot)->L, -1)) { \
        lua_pop((slot)->L, 1); \
        continue; \
    } \
    /* Reset instruction counter so each call gets a fresh 10M budget */ \
    lua_sethook((slot)->L, lua_instruction_hook, LUA_MASKCOUNT, LUA_MAX_INSTRUCTIONS); \
} while(0)

void tb_lua_engine_on_init(tb_lua_engine_t *engine) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_init");

        if (lua_pcall(slot->L, 0, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_init error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_tick(tb_lua_engine_t *engine, const char *coin, double mid_price) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_tick");

        lua_pushstring(slot->L, coin);
        lua_pushnumber(slot->L, mid_price);
        if (lua_pcall(slot->L, 2, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_tick error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_fill(tb_lua_engine_t *engine, const tb_fill_t *fill,
                            const char *strategy_name) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];

        /* Only dispatch fill to the strategy that placed the order */
        if (strategy_name && strcmp(slot->name, strategy_name) != 0)
            continue;

        CALL_LUA_VOID(slot, "on_fill");

        lua_createtable(slot->L, 0, 8);

        lua_pushstring(slot->L, fill->coin);
        lua_setfield(slot->L, -2, "coin");

        lua_pushnumber(slot->L, tb_decimal_to_double(fill->px));
        lua_setfield(slot->L, -2, "price");

        lua_pushnumber(slot->L, tb_decimal_to_double(fill->sz));
        lua_setfield(slot->L, -2, "size");

        lua_pushstring(slot->L, fill->side == TB_SIDE_BUY ? "buy" : "sell");
        lua_setfield(slot->L, -2, "side");

        lua_pushinteger(slot->L, fill->time_ms);
        lua_setfield(slot->L, -2, "time");

        lua_pushnumber(slot->L, tb_decimal_to_double(fill->closed_pnl));
        lua_setfield(slot->L, -2, "closed_pnl");

        lua_pushnumber(slot->L, tb_decimal_to_double(fill->fee));
        lua_setfield(slot->L, -2, "fee");

        lua_pushinteger(slot->L, (lua_Integer)fill->oid);
        lua_setfield(slot->L, -2, "oid");

        if (lua_pcall(slot->L, 1, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_fill error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_timer(tb_lua_engine_t *engine) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_timer");

        if (lua_pcall(slot->L, 0, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_timer error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_book(tb_lua_engine_t *engine, const tb_book_t *book) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_book");

        /* Build book table */
        lua_createtable(slot->L, 0, 3);

        lua_pushstring(slot->L, book->coin);
        lua_setfield(slot->L, -2, "coin");

        /* Bids */
        lua_createtable(slot->L, book->n_bids, 0);
        for (int b = 0; b < book->n_bids; b++) {
            lua_createtable(slot->L, 0, 2);
            lua_pushnumber(slot->L, tb_decimal_to_double(book->bids[b].px));
            lua_setfield(slot->L, -2, "price");
            lua_pushnumber(slot->L, tb_decimal_to_double(book->bids[b].sz));
            lua_setfield(slot->L, -2, "size");
            lua_rawseti(slot->L, -2, b + 1);
        }
        lua_setfield(slot->L, -2, "bids");

        /* Asks */
        lua_createtable(slot->L, book->n_asks, 0);
        for (int a = 0; a < book->n_asks; a++) {
            lua_createtable(slot->L, 0, 2);
            lua_pushnumber(slot->L, tb_decimal_to_double(book->asks[a].px));
            lua_setfield(slot->L, -2, "price");
            lua_pushnumber(slot->L, tb_decimal_to_double(book->asks[a].sz));
            lua_setfield(slot->L, -2, "size");
            lua_rawseti(slot->L, -2, a + 1);
        }
        lua_setfield(slot->L, -2, "asks");

        if (lua_pcall(slot->L, 1, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_book error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_advisory(tb_lua_engine_t *engine, const char *json_adjustments) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_advisory");

        lua_pushstring(slot->L, json_adjustments);
        if (lua_pcall(slot->L, 1, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_advisory error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

void tb_lua_engine_on_shutdown(tb_lua_engine_t *engine) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        tb_strategy_slot_t *slot = &engine->slots[i];
        CALL_LUA_VOID(slot, "on_shutdown");

        if (lua_pcall(slot->L, 0, 0, 0) != LUA_OK) {
            tb_log_error("lua: %s on_shutdown error: %s",
                        slot->name, lua_tostring(slot->L, -1));
            lua_pop(slot->L, 1);
        }
    }
    pthread_mutex_unlock(&engine->lock);
}

/* ── Query ──────────────────────────────────────────────────────────────── */

int tb_lua_engine_get_strategies(const tb_lua_engine_t *engine,
                                  tb_strategy_info_t *out, int *count) {
    int n = engine->n_strategies;
    if (n > *count) n = *count;

    for (int i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%s", engine->slots[i].name);
        snprintf(out[i].path, sizeof(out[i].path), "%s", engine->slots[i].path);
        out[i].loaded = engine->slots[i].loaded;
        out[i].enabled = engine->slots[i].enabled;
        out[i].last_mtime = engine->slots[i].last_mtime;
    }

    *count = n;
    return 0;
}

int tb_lua_engine_set_enabled(tb_lua_engine_t *engine, const char *name, bool enabled) {
    pthread_mutex_lock(&engine->lock);
    for (int i = 0; i < engine->n_strategies; i++) {
        if (strcmp(engine->slots[i].name, name) == 0) {
            engine->slots[i].enabled = enabled;
            tb_log_info("lua: strategy '%s' %s",
                       name, enabled ? "enabled" : "disabled");
            pthread_mutex_unlock(&engine->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&engine->lock);
    tb_log_warn("lua: strategy '%s' not found", name);
    return -1;
}

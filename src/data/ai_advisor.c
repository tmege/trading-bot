#include "data/ai_advisor.h"
#include "strategy/lua_engine.h"
#include "core/logging.h"

#include <curl/curl.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>

struct tb_ai_advisor {
    char              api_key[256];
    char              model[64];
    int               hour_morning;
    int               hour_evening;

    CURL             *curl;
    sqlite3          *db;
    sqlite3_stmt     *insert_stmt;
    tb_lua_engine_t  *lua_engine;

    tb_advisory_context_t  ctx;
    pthread_mutex_t        ctx_lock;

    pthread_t         thread;
    _Atomic bool      running;
    bool              started;
    int64_t           last_call_ts;
    int               last_call_hour;
};

/* ── CURL helpers ───────────────────────────────────────────────────────── */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} tb_curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    tb_curl_buf_t *b = (tb_curl_buf_t *)userdata;
    /* Guard against size_t overflow */
    if (nmemb > 0 && size > SIZE_MAX / nmemb) return 0;
    size_t total = size * nmemb;
    /* Cap buffer at 4 MB to prevent unbounded growth */
    if (b->len + total > 4 * 1024 * 1024) return 0;
    if (b->len + total >= b->cap) {
        size_t new_cap = (b->len + total) * 2 + 1;
        if (new_cap < b->len + total) return 0; /* overflow check */
        char *tmp = realloc(b->buf, new_cap);
        if (!tmp) return 0;
        b->buf = tmp;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->len, ptr, total);
    b->len += total;
    b->buf[b->len] = '\0';
    return total;
}

/* ── Build the prompt from context ──────────────────────────────────────── */
static char *build_prompt(const tb_advisory_context_t *ctx) {
    /* Use yyjson to build a clean JSON context */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* Account */
    yyjson_mut_val *account = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, account, "value", ctx->account_value);
    yyjson_mut_obj_add_real(doc, account, "daily_pnl", ctx->daily_pnl);
    yyjson_mut_obj_add_real(doc, account, "daily_fees", ctx->daily_fees);
    yyjson_mut_obj_add_int(doc, account, "daily_trades", ctx->daily_trades);
    yyjson_mut_obj_add_val(doc, root, "account", account);

    /* Positions */
    yyjson_mut_val *positions = yyjson_mut_arr(doc);
    for (int i = 0; i < ctx->n_positions; i++) {
        yyjson_mut_val *pos = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, pos, "coin", ctx->positions[i].coin);
        yyjson_mut_obj_add_real(doc, pos, "size",
            tb_decimal_to_double(ctx->positions[i].size));
        yyjson_mut_obj_add_real(doc, pos, "entry_px",
            tb_decimal_to_double(ctx->positions[i].entry_px));
        yyjson_mut_obj_add_real(doc, pos, "unrealized_pnl",
            tb_decimal_to_double(ctx->positions[i].unrealized_pnl));
        yyjson_mut_obj_add_int(doc, pos, "leverage", ctx->positions[i].leverage);
        yyjson_mut_arr_add_val(positions, pos);
    }
    yyjson_mut_obj_add_val(doc, root, "positions", positions);

    /* Macro */
    yyjson_mut_val *macro = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, macro, "btc_price", ctx->macro.btc_price);
    yyjson_mut_obj_add_real(doc, macro, "btc_dominance", ctx->macro.btc_dominance);
    yyjson_mut_obj_add_real(doc, macro, "eth_btc", ctx->macro.eth_btc);
    yyjson_mut_obj_add_real(doc, macro, "total2_mcap_b", ctx->macro.total2_mcap);
    yyjson_mut_obj_add_real(doc, macro, "gold", ctx->macro.gold);
    yyjson_mut_obj_add_real(doc, macro, "sp500", ctx->macro.sp500);
    yyjson_mut_obj_add_real(doc, macro, "dxy", ctx->macro.dxy);
    yyjson_mut_obj_add_val(doc, root, "macro", macro);

    /* Sentiment */
    yyjson_mut_val *sentiment = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, sentiment, "score", ctx->sentiment.overall_score);
    yyjson_mut_obj_add_real(doc, sentiment, "bullish_pct", ctx->sentiment.bullish_pct);
    yyjson_mut_obj_add_real(doc, sentiment, "bearish_pct", ctx->sentiment.bearish_pct);
    yyjson_mut_obj_add_int(doc, sentiment, "tweets", ctx->sentiment.total_tweets);
    yyjson_mut_obj_add_val(doc, root, "sentiment", sentiment);

    /* Fear & Greed */
    yyjson_mut_val *fg = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, fg, "value", ctx->fear_greed.value);
    yyjson_mut_obj_add_str(doc, fg, "label", ctx->fear_greed.label);
    yyjson_mut_obj_add_val(doc, root, "fear_greed", fg);

    /* Strategies */
    yyjson_mut_val *strats = yyjson_mut_arr(doc);
    for (int i = 0; i < ctx->n_strategies; i++) {
        yyjson_mut_val *s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, s, "name", ctx->strategies[i].name);
        yyjson_mut_obj_add_bool(doc, s, "enabled", ctx->strategies[i].enabled);
        yyjson_mut_arr_add_val(strats, s);
    }
    yyjson_mut_obj_add_val(doc, root, "strategies", strats);

    /* Recent trades */
    yyjson_mut_val *trades = yyjson_mut_arr(doc);
    for (int i = 0; i < ctx->n_recent_trades; i++) {
        yyjson_mut_val *t = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, t, "coin", ctx->recent_trades[i].coin);
        yyjson_mut_obj_add_str(doc, t, "side", ctx->recent_trades[i].side);
        yyjson_mut_obj_add_real(doc, t, "price", ctx->recent_trades[i].price);
        yyjson_mut_obj_add_real(doc, t, "size", ctx->recent_trades[i].size);
        yyjson_mut_obj_add_real(doc, t, "pnl", ctx->recent_trades[i].pnl);
        yyjson_mut_obj_add_real(doc, t, "fee", ctx->recent_trades[i].fee);
        yyjson_mut_arr_add_val(trades, t);
    }
    yyjson_mut_obj_add_val(doc, root, "recent_trades", trades);

    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(doc);

    if (!json) return NULL;

    /* Build full prompt */
    size_t prompt_len = strlen(json) + 2048;
    char *prompt = malloc(prompt_len);
    if (!prompt) { free(json); return NULL; }

    /* Build dynamic strategy adjustment fields from active strategies */
    char strat_fields[1024] = "";
    for (int i = 0; i < ctx->n_strategies; i++) {
        char line[128];
        snprintf(line, sizeof(line),
            "- %s: {sl_pct, tp_pct, rsi_oversold, rsi_overbought, pause(bool)}\n",
            ctx->strategies[i].name);
        strncat(strat_fields, line, sizeof(strat_fields) - strlen(strat_fields) - 1);
    }

    snprintf(prompt, prompt_len,
        "You are an AI trading advisor for a crypto trading bot on Hyperliquid.\n"
        "Budget: ~100 USDC. Leverage: 5x. Active strategies:\n"
        "1) BB Scalping on multiple coins: Bollinger Band mean reversion, "
        "$40/trade each. RSI<35 at lower BB → long, RSI>65 at upper BB → short, SL 1.5%%, TP 3%%\n"
        "Emergency close at -12 USDC, hard limit at -15 USDC.\n"
        "\n"
        "Current state:\n%s\n"
        "\n"
        "Provide adjustments as JSON with these optional fields:\n"
        "%s"
        "- risk: {daily_limit, max_leverage} — adjust risk parameters\n"
        "- analysis: brief text reasoning (1-2 sentences)\n"
        "\n"
        "Rules:\n"
        "- Only include fields you want to change\n"
        "- Be conservative: small adjustments, never all-in\n"
        "- If losing, tighten stops and reduce size before pausing\n"
        "- NEVER pause BB scalping in Extreme Fear — mean reversion PROFITS from oversold bounces\n"
        "- Only pause scalping if BB width > 8%% (strong trend, not ranging)\n"
        "- Max leverage 5x, max position $300\n"
        "- Momentum is the primary profit driver — avoid pausing unless clear signal\n"
        "- Respond ONLY with valid JSON\n",
        json, strat_fields);

    free(json);
    return prompt;
}

/* ── Build API request body ─────────────────────────────────────────────── */
static char *build_api_body(const char *model, const char *prompt) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", model);
    yyjson_mut_obj_add_int(doc, root, "max_tokens", 1024);

    yyjson_mut_val *messages = yyjson_mut_arr(doc);
    yyjson_mut_val *msg = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, msg, "role", "user");
    yyjson_mut_obj_add_str(doc, msg, "content", prompt);
    yyjson_mut_arr_add_val(messages, msg);
    yyjson_mut_obj_add_val(doc, root, "messages", messages);

    char *body = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return body;
}

/* ── Parse Claude API response ──────────────────────────────────────────── */
static char *parse_response(const char *json_resp, size_t len) {
    yyjson_doc *doc = yyjson_read(json_resp, len, 0);
    if (!doc) return NULL;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return NULL;
    }

    yyjson_val *first = yyjson_arr_get_first(content);
    if (!first) { yyjson_doc_free(doc); return NULL; }

    yyjson_val *text = yyjson_obj_get(first, "text");
    if (!text) { yyjson_doc_free(doc); return NULL; }

    const char *txt = yyjson_get_str(text);
    char *result = txt ? strdup(txt) : NULL;

    yyjson_doc_free(doc);
    return result;
}

/* ── Log advisory to SQLite ─────────────────────────────────────────────── */
static void log_advisory(tb_ai_advisor_t *adv, const char *prompt,
                          const char *response, int success) {
    if (!adv->db || !adv->insert_stmt) return;

    sqlite3_reset(adv->insert_stmt);
    sqlite3_bind_int64(adv->insert_stmt, 1, (int64_t)time(NULL) * 1000);
    sqlite3_bind_text(adv->insert_stmt, 2, prompt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(adv->insert_stmt, 3, response ? response : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(adv->insert_stmt, 4, success);
    sqlite3_step(adv->insert_stmt);
}

/* ── Call Claude API ────────────────────────────────────────────────────── */
static int call_claude(tb_ai_advisor_t *adv) {
    pthread_mutex_lock(&adv->ctx_lock);
    tb_advisory_context_t ctx = adv->ctx;
    pthread_mutex_unlock(&adv->ctx_lock);

    /* Build prompt */
    char *prompt = build_prompt(&ctx);
    if (!prompt) {
        tb_log_error("advisory: failed to build prompt");
        return -1;
    }

    /* Build API body */
    char *body = build_api_body(adv->model, prompt);
    if (!body) {
        free(prompt);
        tb_log_error("advisory: failed to build API body");
        return -1;
    }

    tb_log_info("advisory: calling Claude %s...", adv->model);

    /* Reset curl handle to avoid leaking options from previous calls */
    curl_easy_reset(adv->curl);

    /* HTTP request */
    tb_curl_buf_t resp = { .buf = malloc(16384), .len = 0, .cap = 16384 };
    if (!resp.buf) { free(prompt); free(body); return -1; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", adv->api_key);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(adv->curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(adv->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(adv->curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(adv->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(adv->curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(adv->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(adv->curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(adv->curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(adv->curl);
    curl_slist_free_all(headers);
    /* Wipe API key from stack buffer */
    {
        static void *(*const volatile memset_fn)(void *, int, size_t) = memset;
        (memset_fn)(auth_header, 0, sizeof(auth_header));
    }

    if (res != CURLE_OK) {
        tb_log_error("advisory: API call failed: %s", curl_easy_strerror(res));
        log_advisory(adv, prompt, NULL, 0);
        free(prompt); free(body); free(resp.buf);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(adv->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        tb_log_error("advisory: API HTTP %ld: %.200s", http_code, resp.buf);
        log_advisory(adv, prompt, resp.buf, 0);
        free(prompt); free(body); free(resp.buf);
        return -1;
    }

    /* Parse response text */
    char *ai_response = parse_response(resp.buf, resp.len);
    if (!ai_response) {
        tb_log_error("advisory: failed to parse response");
        log_advisory(adv, prompt, resp.buf, 0);
        free(prompt); free(body); free(resp.buf);
        return -1;
    }

    /* Strip markdown code fences (```json ... ```) if present */
    char *json_start = ai_response;
    size_t ai_len = strlen(ai_response);
    char *fence = strstr(ai_response, "```");
    if (fence && (size_t)(fence - ai_response) + 3 < ai_len) {
        /* Skip opening fence + optional language tag + newline */
        json_start = fence + 3;
        while (*json_start && *json_start != '\n') json_start++;
        if (*json_start == '\n') json_start++;

        /* Find closing fence and terminate there */
        char *fence_end = strstr(json_start, "```");
        if (fence_end) *fence_end = '\0';

        /* Trim leading/trailing whitespace */
        while (*json_start == ' ' || *json_start == '\t' || *json_start == '\n')
            json_start++;
        size_t jlen = strlen(json_start);
        while (jlen > 0 && (json_start[jlen-1] == ' ' || json_start[jlen-1] == '\n'
                            || json_start[jlen-1] == '\r'))
            json_start[--jlen] = '\0';

        /* Replace ai_response with cleaned JSON */
        char *cleaned = strdup(json_start);
        if (cleaned) {
            free(ai_response);
            ai_response = cleaned;
        }
    }

    tb_log_info("advisory: response: %.500s", ai_response);

    /* Log to SQLite */
    log_advisory(adv, prompt, ai_response, 1);

    /* Dispatch to Lua strategies */
    if (adv->lua_engine) {
        tb_lua_engine_on_advisory(adv->lua_engine, ai_response);
    }

    adv->last_call_ts = (int64_t)time(NULL) * 1000;

    free(ai_response);
    free(prompt);
    free(body);
    free(resp.buf);

    return 0;
}

/* ── Background thread ──────────────────────────────────────────────────── */
static void *advisor_thread_func(void *arg) {
    tb_ai_advisor_t *adv = (tb_ai_advisor_t *)arg;
    tb_log_info("advisory: background thread started (hours: %02d:00, %02d:00 UTC)",
                adv->hour_morning, adv->hour_evening);

    while (adv->running) {
        time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        int current_hour = utc.tm_hour;

        /* Check if it's time for a call */
        bool should_call = false;
        if ((current_hour == adv->hour_morning || current_hour == adv->hour_evening)
            && current_hour != adv->last_call_hour) {
            should_call = true;
        }

        if (should_call && adv->api_key[0] != '\0') {
            adv->last_call_hour = current_hour;
            call_claude(adv);
        }

        /* Sleep 60s between checks */
        for (int i = 0; i < 60 && adv->running; i++) {
            usleep(1000000);
        }
    }

    tb_log_info("advisory: background thread stopped");
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_ai_advisor_t *tb_ai_advisor_create(const tb_config_t *cfg, sqlite3 *db) {
    tb_ai_advisor_t *adv = calloc(1, sizeof(tb_ai_advisor_t));
    if (!adv) return NULL;

    snprintf(adv->api_key, sizeof(adv->api_key), "%s", cfg->claude_api_key);
    snprintf(adv->model, sizeof(adv->model), "%s",
             cfg->claude_model[0] ? cfg->claude_model : "claude-haiku-4-5-20251001");
    adv->hour_morning = cfg->advisory_hour_morning > 0 ? cfg->advisory_hour_morning : 8;
    adv->hour_evening = cfg->advisory_hour_evening > 0 ? cfg->advisory_hour_evening : 20;
    adv->last_call_hour = -1;

    adv->curl = curl_easy_init();
    if (!adv->curl) { free(adv); return NULL; }

    adv->db = db;
    pthread_mutex_init(&adv->ctx_lock, NULL);

    /* Prepare insert statement */
    if (db) {
        const char *sql = "INSERT INTO advisory_log (timestamp_ms, prompt_text, response_text, adjustments) "
                          "VALUES (?, ?, ?, ?)";
        sqlite3_prepare_v2(db, sql, -1, &adv->insert_stmt, NULL);
    }

    tb_log_info("advisory: created (model=%s, hours=%02d/%02d UTC, api_key=%s)",
                adv->model, adv->hour_morning, adv->hour_evening,
                adv->api_key[0] ? "set" : "MISSING");

    if (adv->api_key[0] == '\0') {
        tb_log_warn("advisory: no API key — set TB_CLAUDE_API_KEY env var");
    }

    return adv;
}

void tb_ai_advisor_destroy(tb_ai_advisor_t *adv) {
    if (!adv) return;
    if (adv->started) tb_ai_advisor_stop(adv);
    if (adv->insert_stmt) sqlite3_finalize(adv->insert_stmt);
    if (adv->curl) curl_easy_cleanup(adv->curl);
    pthread_mutex_destroy(&adv->ctx_lock);
    /* Secure wipe API key before freeing */
    volatile unsigned char *p = (volatile unsigned char *)adv->api_key;
    for (size_t i = 0; i < sizeof(adv->api_key); i++) p[i] = 0;
    free(adv);
}

void tb_ai_advisor_set_lua_engine(tb_ai_advisor_t *adv, tb_lua_engine_t *engine) {
    adv->lua_engine = engine;
}

int tb_ai_advisor_start(tb_ai_advisor_t *adv) {
    if (adv->started) return 0;

    adv->running = true;
    if (pthread_create(&adv->thread, NULL, advisor_thread_func, adv) != 0) {
        tb_log_error("advisory: failed to start thread");
        adv->running = false;
        return -1;
    }

    adv->started = true;
    return 0;
}

void tb_ai_advisor_stop(tb_ai_advisor_t *adv) {
    if (!adv->started) return;
    adv->running = false;
    pthread_join(adv->thread, NULL);
    adv->started = false;
}

void tb_ai_advisor_update_context(tb_ai_advisor_t *adv, const tb_advisory_context_t *ctx) {
    pthread_mutex_lock(&adv->ctx_lock);
    adv->ctx = *ctx;
    pthread_mutex_unlock(&adv->ctx_lock);
}

int tb_ai_advisor_call_now(tb_ai_advisor_t *adv) {
    if (adv->api_key[0] == '\0') {
        tb_log_warn("advisory: cannot call — no API key");
        return -1;
    }
    return call_claude(adv);
}

int64_t tb_ai_advisor_last_call_time(const tb_ai_advisor_t *adv) {
    return adv->last_call_ts;
}

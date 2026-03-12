/*
 * Candle Fetcher — Pre-download historical candles to local SQLite cache
 *
 * Fetches candles from Binance for multiple coins and intervals,
 * stores them in data/candle_cache.db for instant backtest access.
 *
 * Usage: ./candle_fetcher [--days N] [--coins ETH,BTC,...] [--intervals 5m,15m,...]
 *
 * Defaults: 6 coins (ETH,BTC,SOL,DOGE,HYPE,PUMP), all intervals, 4 years (1461 days)
 * Incremental: only fetches candles newer than what's already cached.
 * Uses Binance /api/v3/klines — no API key required.
 */

#include "core/types.h"
#include "core/logging.h"

#include <sqlite3.h>
#include <curl/curl.h>
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

/* ── Defaults ──────────────────────────────────────────────────────────── */
#define DEFAULT_DAYS      1461  /* 4 years */
#define MAX_COINS         16
#define MAX_INTERVALS     8
#define BATCH_SIZE        500
#define RATE_LIMIT_US     200000  /* 200ms between API calls */
#define MAX_CANDLES_ALLOC 200000

/* ── Binance fallback ─────────────────────────────────────────────────── */
#define BINANCE_BASE_URL  "https://api.binance.com"
#define BINANCE_BATCH     1000   /* max per request */
#define BINANCE_RATE_US   200000 /* 200ms between requests */

static const char *DEFAULT_COINS[] = {"ETH", "BTC", "SOL", "DOGE", "HYPE", "PUMP", NULL};
static const char *DEFAULT_INTERVALS[] = {"5m", "15m", "1h", "4h", "1d", NULL};

/* ── Interval to milliseconds ──────────────────────────────────────────── */
static int64_t interval_to_ms(const char *interval) {
    if (strcmp(interval, "1m") == 0)  return 60000LL;
    if (strcmp(interval, "5m") == 0)  return 300000LL;
    if (strcmp(interval, "15m") == 0) return 900000LL;
    if (strcmp(interval, "30m") == 0) return 1800000LL;
    if (strcmp(interval, "1h") == 0)  return 3600000LL;
    if (strcmp(interval, "4h") == 0)  return 14400000LL;
    if (strcmp(interval, "1d") == 0)  return 86400000LL;
    return 3600000LL;
}

/* ── Progress output ───────────────────────────────────────────────────── */
static void progress(const char *coin, const char *interval,
                     int fetched, int total_so_far) {
    fprintf(stderr, "{\"coin\":\"%s\",\"interval\":\"%s\",\"fetched\":%d,\"total\":%d}\n",
            coin, interval, fetched, total_so_far);
    fflush(stderr);
}

/* ── Create/open cache database ────────────────────────────────────────── */
static sqlite3 *open_cache_db(const char *db_path) {
    /* Ensure data/ directory exists */
    char dir[512];
    strncpy(dir, db_path, sizeof(dir) - 1);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0700);
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        return NULL;
    }

    /* WAL mode for concurrent reads during backtest */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Create table */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS candles ("
        "  coin      TEXT NOT NULL,"
        "  interval  TEXT NOT NULL,"
        "  time_ms   INTEGER NOT NULL,"
        "  open      REAL NOT NULL,"
        "  high      REAL NOT NULL,"
        "  low       REAL NOT NULL,"
        "  close     REAL NOT NULL,"
        "  volume    REAL NOT NULL,"
        "  PRIMARY KEY (coin, interval, time_ms)"
        ")";
    char *err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ERROR: create table: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    /* Index for fast range queries */
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_candles_lookup "
        "ON candles(coin, interval, time_ms)",
        NULL, NULL, NULL);

    /* Metadata table for sync tracking */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sync_meta ("
        "  coin      TEXT NOT NULL,"
        "  interval  TEXT NOT NULL,"
        "  last_sync_ms INTEGER NOT NULL,"
        "  n_candles INTEGER NOT NULL,"
        "  PRIMARY KEY (coin, interval)"
        ")",
        NULL, NULL, NULL);

    return db;
}

/* ── Get latest cached timestamp for a coin/interval ───────────────────── */
static int64_t get_latest_cached(sqlite3 *db, const char *coin, const char *interval) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT MAX(time_ms) FROM candles WHERE coin=? AND interval=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);

    int64_t latest = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        latest = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return latest;
}

/* ── Get earliest cached candle timestamp ──────────────────────────────── */
static int64_t get_earliest_cached(sqlite3 *db, const char *coin, const char *interval) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT MIN(time_ms) FROM candles WHERE coin=? AND interval=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);

    int64_t earliest = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        earliest = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return earliest;
}

/* ── Get total cached count ────────────────────────────────────────────── */
static int get_cached_count(sqlite3 *db, const char *coin, const char *interval) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM candles WHERE coin=? AND interval=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ── Insert candles in batch ───────────────────────────────────────────── */
static int insert_candles(sqlite3 *db, const char *coin, const char *interval,
                           const tb_candle_t *candles, int n) {
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO candles (coin, interval, time_ms, open, high, low, close, volume) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, candles[i].time_open);
        sqlite3_bind_double(stmt, 4, tb_decimal_to_double(candles[i].open));
        sqlite3_bind_double(stmt, 5, tb_decimal_to_double(candles[i].high));
        sqlite3_bind_double(stmt, 6, tb_decimal_to_double(candles[i].low));
        sqlite3_bind_double(stmt, 7, tb_decimal_to_double(candles[i].close));
        sqlite3_bind_double(stmt, 8, tb_decimal_to_double(candles[i].volume));
        sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

/* ── Update sync metadata ──────────────────────────────────────────────── */
static void update_sync_meta(sqlite3 *db, const char *coin, const char *interval,
                              int64_t last_sync_ms, int n_candles) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO sync_meta (coin, interval, last_sync_ms, n_candles) "
        "VALUES (?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, last_sync_ms);
    sqlite3_bind_int(stmt, 4, n_candles);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ── Binance curl helpers ──────────────────────────────────────────────── */
typedef struct {
    char   *data;
    size_t  size;
} bn_response_t;

static size_t bn_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    if (nmemb > 0 && size > SIZE_MAX / nmemb) return 0;
    size_t total = size * nmemb;
    bn_response_t *buf = (bn_response_t *)userp;
    if (buf->size + total > 16 * 1024 * 1024) return 0;
    char *new_data = realloc(buf->data, buf->size + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static CURL *bn_curl = NULL;

static CURL *bn_get_curl(void) {
    if (!bn_curl) {
        bn_curl = curl_easy_init();
    }
    return bn_curl;
}

/* Fetch up to BINANCE_BATCH candles from Binance /api/v3/klines */
static int binance_get_candles(const char *coin, const char *interval,
                                int64_t start_ms, int64_t end_ms,
                                tb_candle_t *out, int *out_count, int max_count) {
    *out_count = 0;

    /* Build symbol: ETH → ETHUSDT */
    char symbol[32];
    snprintf(symbol, sizeof(symbol), "%sUSDT", coin);

    /* Build URL */
    char url[512];
    int limit = max_count < BINANCE_BATCH ? max_count : BINANCE_BATCH;
    snprintf(url, sizeof(url),
             "%s/api/v3/klines?symbol=%s&interval=%s"
             "&startTime=%lld&endTime=%lld&limit=%d",
             BINANCE_BASE_URL, symbol, interval,
             (long long)start_ms, (long long)end_ms, limit);

    CURL *curl = bn_get_curl();
    if (!curl) return -1;

    bn_response_t resp = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bn_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "  Binance curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "  Binance HTTP %ld: %.200s\n", http_code,
                resp.data ? resp.data : "(null)");
        free(resp.data);
        return -1;
    }

    /* Parse JSON: array of arrays */
    yyjson_doc *doc = yyjson_read(resp.data, resp.size, 0);
    free(resp.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return -1;
    }

    int n = 0;
    size_t idx, max_iter;
    yyjson_val *kline;
    yyjson_arr_foreach(root, idx, max_iter, kline) {
        if (n >= max_count) break;
        if (!yyjson_is_arr(kline) || yyjson_arr_size(kline) < 9) continue;

        const char *s_open  = yyjson_get_str(yyjson_arr_get(kline, 1));
        const char *s_high  = yyjson_get_str(yyjson_arr_get(kline, 2));
        const char *s_low   = yyjson_get_str(yyjson_arr_get(kline, 3));
        const char *s_close = yyjson_get_str(yyjson_arr_get(kline, 4));
        const char *s_vol   = yyjson_get_str(yyjson_arr_get(kline, 5));
        if (!s_open || !s_high || !s_low || !s_close || !s_vol) continue;

        tb_candle_t *c = &out[n];
        c->time_open  = yyjson_get_sint(yyjson_arr_get(kline, 0));
        c->time_close = yyjson_get_sint(yyjson_arr_get(kline, 6));
        c->open       = tb_decimal_from_str(s_open);
        c->high       = tb_decimal_from_str(s_high);
        c->low        = tb_decimal_from_str(s_low);
        c->close      = tb_decimal_from_str(s_close);
        c->volume     = tb_decimal_from_str(s_vol);
        c->n_trades   = (int)yyjson_get_sint(yyjson_arr_get(kline, 8));
        n++;
    }

    yyjson_doc_free(doc);
    *out_count = n;
    return 0;
}

/* Fetch range via Binance with pagination */
static int fetch_range(sqlite3 *db, const char *coin, const char *interval,
                       int64_t from_ms, int64_t to_ms) {
    int64_t candle_ms = interval_to_ms(interval);
    tb_candle_t *candles = calloc(BINANCE_BATCH, sizeof(tb_candle_t));
    if (!candles) return -1;

    int total_fetched = 0;
    int64_t cursor = from_ms;

    while (cursor < to_ms) {
        int batch_count = 0;
        int rc = binance_get_candles(coin, interval, cursor, to_ms,
                                      candles, &batch_count, BINANCE_BATCH);
        if (rc != 0 || batch_count == 0) {
            if (total_fetched > 0) break;
            free(candles);
            return -1;
        }

        insert_candles(db, coin, interval, candles, batch_count);
        total_fetched += batch_count;

        cursor = candles[batch_count - 1].time_open + candle_ms + 1;

        progress(coin, interval, batch_count, total_fetched);

        if (cursor < to_ms) usleep(BINANCE_RATE_US);
    }

    free(candles);
    return total_fetched;
}

static int fetch_and_store(sqlite3 *db,
                            const char *coin, const char *interval,
                            int n_days) {
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t want_start_ms = now_ms - (int64_t)n_days * 86400000LL;

    int64_t earliest = get_earliest_cached(db, coin, interval);
    int64_t latest   = get_latest_cached(db, coin, interval);
    int total_fetched = 0;

    /* Phase 1: Backfill — fetch older candles if cache doesn't go back far enough */
    if (earliest > 0 && want_start_ms < earliest) {
        fprintf(stderr, "  %s/%s: backfilling from %lld to %lld\n",
                coin, interval,
                (long long)(want_start_ms / 1000),
                (long long)(earliest / 1000));
        int bf = fetch_range(db, coin, interval, want_start_ms, earliest);
        if (bf > 0) total_fetched += bf;
    }

    /* Phase 2: Forward fill — fetch newer candles from latest cached */
    int64_t fwd_start = want_start_ms;
    if (latest > 0) {
        int64_t candle_ms = interval_to_ms(interval);
        fwd_start = latest + candle_ms;
    }

    if (fwd_start < now_ms) {
        int ff = fetch_range(db, coin, interval, fwd_start, now_ms);
        if (ff > 0) total_fetched += ff;
    }

    if (total_fetched == 0 && earliest > 0) {
        int cached = get_cached_count(db, coin, interval);
        fprintf(stderr, "  %s/%s: up to date (%d candles cached)\n",
                coin, interval, cached);
    }

    /* Update sync metadata */
    int total_cached = get_cached_count(db, coin, interval);
    update_sync_meta(db, coin, interval, now_ms, total_cached);

    return total_fetched;
}

/* ── Parse comma-separated list ────────────────────────────────────────── */
static int parse_list(const char *input, const char **out, int max) {
    static char bufs[2][512];
    static int buf_idx = 0;
    char *buf = bufs[buf_idx++ % 2];
    strncpy(buf, input, 511);
    buf[511] = '\0';
    int n = 0;
    char *tok = strtok(buf, ",");
    while (tok && n < max) {
        out[n++] = tok;
        tok = strtok(NULL, ",");
    }
    out[n] = NULL;
    return n;
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init("./logs", TB_LOG_LVL_WARN);

    int n_days = DEFAULT_DAYS;
    const char *coins[MAX_COINS + 1];
    const char *intervals[MAX_INTERVALS + 1];
    const char *db_path = "./data/candle_cache.db";

    /* Copy defaults */
    for (int i = 0; DEFAULT_COINS[i]; i++) coins[i] = DEFAULT_COINS[i];
    coins[6] = NULL;
    for (int i = 0; DEFAULT_INTERVALS[i]; i++) intervals[i] = DEFAULT_INTERVALS[i];
    intervals[5] = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
            n_days = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--coins") == 0 && i + 1 < argc) {
            parse_list(argv[++i], coins, MAX_COINS);
        } else if (strcmp(argv[i], "--intervals") == 0 && i + 1 < argc) {
            parse_list(argv[++i], intervals, MAX_INTERVALS);
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        }
    }

    /* Open cache DB */
    sqlite3 *db = open_cache_db(db_path);
    if (!db) return 1;

    /* Count work */
    int n_coins = 0, n_intervals = 0;
    for (int i = 0; coins[i]; i++) n_coins++;
    for (int i = 0; intervals[i]; i++) n_intervals++;
    int total_jobs = n_coins * n_intervals;
    int done = 0;
    int grand_total = 0;

    fprintf(stderr, "Fetching %d days of candles for %d coins x %d intervals (%d jobs)\n",
            n_days, n_coins, n_intervals, total_jobs);

    /* Fetch all combinations */
    for (int c = 0; coins[c]; c++) {
        for (int v = 0; intervals[v]; v++) {
            done++;
            fprintf(stderr, "\n[%d/%d] %s / %s\n", done, total_jobs, coins[c], intervals[v]);

            int fetched = fetch_and_store(db, coins[c], intervals[v], n_days);
            if (fetched < 0) {
                fprintf(stderr, "  WARNING: failed to fetch %s/%s\n", coins[c], intervals[v]);
            } else {
                grand_total += fetched;
            }
        }
    }

    /* Print summary */
    fprintf(stderr, "\n=== SYNC COMPLETE ===\n");
    fprintf(stderr, "New candles fetched: %d\n", grand_total);
    fprintf(stderr, "Cache database: %s\n\n", db_path);

    /* Print per-coin/interval counts as JSON on stdout */
    printf("{\n");
    printf("  \"status\": \"complete\",\n");
    printf("  \"new_candles\": %d,\n", grand_total);
    printf("  \"cache\": [\n");
    int first = 1;
    for (int c = 0; coins[c]; c++) {
        for (int v = 0; intervals[v]; v++) {
            int count = get_cached_count(db, coins[c], intervals[v]);
            if (!first) printf(",\n");
            printf("    {\"coin\":\"%s\",\"interval\":\"%s\",\"candles\":%d}",
                   coins[c], intervals[v], count);
            first = 0;
        }
    }
    printf("\n  ]\n}\n");

    if (bn_curl) curl_easy_cleanup(bn_curl);
    sqlite3_close(db);
    return 0;
}

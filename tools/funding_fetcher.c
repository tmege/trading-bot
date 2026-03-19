/*
 * Funding Rate Fetcher — Download historical funding rates from Binance
 *
 * Fetches funding rates from Binance Futures API (fapi/v1/fundingRate),
 * stores them in the same SQLite cache database as candles.
 * Public endpoint, no API key required.
 *
 * Usage: ./funding_fetcher --coins BTC,ETH,SOL,DOGE --days 3200
 *
 * Incremental: only fetches rates newer than what's already cached.
 */

#include <curl/curl.h>
#include <yyjson.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

/* ── Config ────────────────────────────────────────────────────────────── */
#define DEFAULT_DAYS      3200
#define MAX_COINS         16
#define BATCH_SIZE        1000
#define RATE_LIMIT_US     200000  /* 200ms between requests */
#define CACHE_DB_PATH     "./data/candle_cache.db"
#define BINANCE_FR_URL    "https://fapi.binance.com/fapi/v1/fundingRate"

static const char *DEFAULT_COINS[] = {"BTC", "ETH", "SOL", "DOGE", NULL};

/* ── CURL helpers ──────────────────────────────────────────────────────── */
typedef struct {
    char   *data;
    size_t  size;
} response_t;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    if (nmemb > 0 && size > SIZE_MAX / nmemb) return 0;
    size_t total = size * nmemb;
    response_t *buf = (response_t *)userp;
    if (buf->size + total > 16 * 1024 * 1024) return 0;
    char *new_data = realloc(buf->data, buf->size + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static CURL *g_curl = NULL;

static CURL *get_curl(void) {
    if (!g_curl) g_curl = curl_easy_init();
    return g_curl;
}

/* ── SQLite setup ──────────────────────────────────────────────────────── */
static sqlite3 *open_db(const char *db_path) {
    /* Ensure directory exists */
    char dir[512];
    strncpy(dir, db_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        return NULL;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    const char *sql =
        "CREATE TABLE IF NOT EXISTS funding_rates ("
        "  coin       TEXT NOT NULL,"
        "  time_ms    INTEGER NOT NULL,"
        "  rate       REAL NOT NULL,"
        "  mark_price REAL NOT NULL,"
        "  PRIMARY KEY (coin, time_ms)"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "ERROR: create table: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_fr_lookup "
        "ON funding_rates(coin, time_ms)",
        NULL, NULL, NULL);

    return db;
}

/* ── Get last cached timestamp ─────────────────────────────────────────── */
static int64_t get_last_time_ms(sqlite3 *db, const char *coin) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT MAX(time_ms) FROM funding_rates WHERE coin=?",
        -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    int64_t last = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        last = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return last;
}

/* ── Fetch one batch from Binance ──────────────────────────────────────── */
static int fetch_batch(const char *symbol, int64_t start_time,
                       int64_t *out_times, double *out_rates,
                       double *out_marks, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s?symbol=%s&startTime=%lld&limit=%d",
        BINANCE_FR_URL, symbol, (long long)start_time, BATCH_SIZE);

    CURL *curl = get_curl();
    if (!curl) return -1;

    response_t resp = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "trading-bot/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "  curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 429) {
        fprintf(stderr, "  Rate limited, sleeping 5s...\n");
        free(resp.data);
        usleep(5000000);
        return fetch_batch(symbol, start_time, out_times, out_rates, out_marks, max);
    }

    if (http_code != 200) {
        fprintf(stderr, "  HTTP %ld: %.200s\n", http_code,
                resp.data ? resp.data : "(null)");
        free(resp.data);
        return -1;
    }

    /* Parse JSON array */
    yyjson_doc *doc = yyjson_read(resp.data, resp.size, 0);
    free(resp.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return -1;
    }

    int n = 0;
    size_t idx, arr_max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, arr_max, item) {
        if (n >= max) break;

        yyjson_val *v_time = yyjson_obj_get(item, "fundingTime");
        yyjson_val *v_rate = yyjson_obj_get(item, "fundingRate");
        yyjson_val *v_mark = yyjson_obj_get(item, "markPrice");

        if (!v_time || !v_rate) continue;

        out_times[n] = yyjson_get_sint(v_time);
        const char *rate_str = yyjson_get_str(v_rate);
        out_rates[n] = rate_str ? atof(rate_str) : 0.0;
        const char *mark_str = v_mark ? yyjson_get_str(v_mark) : NULL;
        out_marks[n] = mark_str ? atof(mark_str) : 0.0;
        n++;
    }

    yyjson_doc_free(doc);
    return n;
}

/* ── Download all funding rates for one coin ───────────────────────────── */
static int download_coin(sqlite3 *db, const char *coin, int64_t start_ms) {
    char symbol[32];
    snprintf(symbol, sizeof(symbol), "%sUSDT", coin);

    int64_t last_ms = get_last_time_ms(db, coin);
    int64_t cursor;

    if (last_ms > 0) {
        cursor = last_ms + 1;
        time_t t = (time_t)(last_ms / 1000);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t));
        fprintf(stderr, "  Incremental from %s\n", buf);
    } else {
        cursor = start_ms;
        time_t t = (time_t)(start_ms / 1000);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t));
        fprintf(stderr, "  Full download from %s\n", buf);
    }

    /* Batch buffers */
    int64_t *times = malloc(BATCH_SIZE * sizeof(int64_t));
    double  *rates = malloc(BATCH_SIZE * sizeof(double));
    double  *marks = malloc(BATCH_SIZE * sizeof(double));
    if (!times || !rates || !marks) {
        free(times); free(rates); free(marks);
        return -1;
    }

    /* Prepared insert statement */
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO funding_rates (coin, time_ms, rate, mark_price) "
        "VALUES (?, ?, ?, ?)", -1, &ins, NULL) != SQLITE_OK) {
        free(times); free(rates); free(marks);
        return -1;
    }

    int total_inserted = 0;
    int batch_num = 0;

    while (1) {
        int n = fetch_batch(symbol, cursor, times, rates, marks, BATCH_SIZE);
        if (n <= 0) break;

        /* Insert batch */
        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        for (int i = 0; i < n; i++) {
            sqlite3_reset(ins);
            sqlite3_bind_text(ins, 1, coin, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins, 2, times[i]);
            sqlite3_bind_double(ins, 3, rates[i]);
            sqlite3_bind_double(ins, 4, marks[i]);
            sqlite3_step(ins);
        }
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        total_inserted += n;

        batch_num++;
        if (batch_num % 10 == 0) {
            fprintf(stderr, "    %d records...\n", total_inserted);
        }

        /* Advance cursor */
        int64_t last_in_batch = times[0];
        for (int i = 1; i < n; i++) {
            if (times[i] > last_in_batch) last_in_batch = times[i];
        }
        if (last_in_batch <= cursor) break; /* no progress */
        cursor = last_in_batch + 1;

        /* End of data? */
        if (n < BATCH_SIZE) break;

        usleep(RATE_LIMIT_US);
    }

    sqlite3_finalize(ins);
    free(times);
    free(rates);
    free(marks);

    fprintf(stderr, "  %s: %d new records inserted\n", coin, total_inserted);
    return total_inserted;
}

/* ── Parse comma-separated list ────────────────────────────────────────── */
static int parse_list(const char *input, const char **out, int max) {
    static char buf[512];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
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
    int n_days = DEFAULT_DAYS;
    const char *coins[MAX_COINS + 1];
    const char *db_path = CACHE_DB_PATH;

    /* Defaults */
    for (int i = 0; DEFAULT_COINS[i]; i++) coins[i] = DEFAULT_COINS[i];
    coins[4] = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
            n_days = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--coins") == 0 && i + 1 < argc) {
            parse_list(argv[++i], coins, MAX_COINS);
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [--coins BTC,ETH,...] [--days N] [--db path]\n", argv[0]);
            return 0;
        }
    }

    sqlite3 *db = open_db(db_path);
    if (!db) return 1;

    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t start_ms = now_ms - (int64_t)n_days * 86400LL * 1000LL;

    fprintf(stderr, "Funding Rate Fetcher\n");
    fprintf(stderr, "  DB: %s\n", db_path);
    fprintf(stderr, "  Coins: ");
    for (int i = 0; coins[i]; i++)
        fprintf(stderr, "%s%s", coins[i], coins[i+1] ? ", " : "\n");
    fprintf(stderr, "  Period: %d days\n\n", n_days);

    int grand_total = 0;
    for (int i = 0; coins[i]; i++) {
        fprintf(stderr, "[%s]\n", coins[i]);
        int n = download_coin(db, coins[i], start_ms);
        if (n > 0) grand_total += n;
        fprintf(stderr, "\n");
    }

    /* Summary */
    fprintf(stderr, "Summary:\n");
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT coin, COUNT(*), MIN(time_ms), MAX(time_ms) "
        "FROM funding_rates GROUP BY coin ORDER BY coin",
        -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *c = (const char *)sqlite3_column_text(stmt, 0);
            int count = sqlite3_column_int(stmt, 1);
            int64_t min_t = sqlite3_column_int64(stmt, 2);
            int64_t max_t = sqlite3_column_int64(stmt, 3);
            char s1[32], s2[32];
            time_t t1 = (time_t)(min_t / 1000), t2 = (time_t)(max_t / 1000);
            strftime(s1, sizeof(s1), "%Y-%m-%d", gmtime(&t1));
            strftime(s2, sizeof(s2), "%Y-%m-%d", gmtime(&t2));
            fprintf(stderr, "  %s: %d records (%s -> %s)\n", c, count, s1, s2);
        }
        sqlite3_finalize(stmt);
    }

    fprintf(stderr, "\nTotal: %d new records inserted\n", grand_total);

    if (g_curl) curl_easy_cleanup(g_curl);
    sqlite3_close(db);
    return 0;
}

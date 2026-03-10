#include "data/macro_fetcher.h"
#include "exchange/hl_rest.h"
#include "core/logging.h"
#include "core/types.h"

#include <curl/curl.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

struct tb_macro_fetcher {
    char             api_key[256];  /* for premium endpoints if needed */
    CURL            *curl;
    hl_rest_t       *hl_rest;       /* optional: Hyperliquid for crypto prices */
    tb_macro_data_t  data;
    pthread_mutex_t  lock;
};

/* ── CURL write callback ────────────────────────────────────────────────── */
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

static int fetch_url(CURL *curl, const char *url, tb_curl_buf_t *buf) {
    buf->len = 0;
    if (buf->buf) buf->buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "trading-bot/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        tb_log_warn("macro: fetch failed: %s", curl_easy_strerror(res));
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        tb_log_warn("macro: HTTP %ld", http_code);
        return -1;
    }

    return 0;
}

/* Helper: get number from yyjson val (handles both int and real) */
static double get_json_number(yyjson_val *val) {
    if (!val) return 0.0;
    if (yyjson_is_real(val)) return yyjson_get_real(val);
    if (yyjson_is_int(val))  return (double)yyjson_get_int(val);
    if (yyjson_is_uint(val)) return (double)yyjson_get_uint(val);
    return 0.0;
}

/* ── Hyperliquid: BTC, ETH prices (real-time, free, no rate limit issue) ── */
static int fetch_hl_crypto(tb_macro_fetcher_t *f) {
    if (!f->hl_rest) return -1;

    tb_mid_t mids[512];
    int count = 0;
    if (hl_rest_get_all_mids(f->hl_rest, mids, &count) != 0) {
        tb_log_warn("macro: HL allMids fetch failed, falling back to CoinGecko");
        return -1;
    }

    double btc_price = 0, eth_price = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(mids[i].coin, "BTC") == 0) {
            btc_price = tb_decimal_to_double(mids[i].mid);
        } else if (strcmp(mids[i].coin, "ETH") == 0) {
            eth_price = tb_decimal_to_double(mids[i].mid);
        }
    }

    if (btc_price > 0) {
        f->data.btc_price = btc_price;
        if (eth_price > 0) {
            f->data.eth_btc = eth_price / btc_price;
        }
    }

    tb_log_debug("macro: HL prices — BTC=$%.0f ETH=$%.0f ETH/BTC=%.5f",
                 btc_price, eth_price, f->data.eth_btc);
    return 0;
}

/* ── CoinGecko: dominance + total2 (not available on HL) ────────────────── */
static int fetch_coingecko_macro(tb_macro_fetcher_t *f, tb_curl_buf_t *buf) {
    if (fetch_url(f->curl,
                  "https://api.coingecko.com/api/v3/global",
                  buf) == 0 && buf->len > 0) {
        yyjson_doc *doc = yyjson_read(buf->buf, buf->len, 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            yyjson_val *data = yyjson_obj_get(root, "data");
            if (data) {
                yyjson_val *mcap_pct = yyjson_obj_get(data, "market_cap_percentage");
                if (mcap_pct) {
                    yyjson_val *btc_dom = yyjson_obj_get(mcap_pct, "btc");
                    if (btc_dom) f->data.btc_dominance = get_json_number(btc_dom);
                }
                yyjson_val *total_mcap = yyjson_obj_get(data, "total_market_cap");
                if (total_mcap) {
                    yyjson_val *usd = yyjson_obj_get(total_mcap, "usd");
                    if (usd) {
                        double total = get_json_number(usd);
                        double btc_pct = f->data.btc_dominance / 100.0;
                        f->data.total2_mcap = (total * (1.0 - btc_pct)) / 1e9;
                    }
                }
            }
            yyjson_doc_free(doc);
        }
    }

    return 0;
}

/* ── CoinGecko fallback: BTC + ETH prices (only if HL not available) ────── */
static int fetch_coingecko_prices(tb_macro_fetcher_t *f, tb_curl_buf_t *buf) {
    if (fetch_url(f->curl,
                  "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum&vs_currencies=usd,btc",
                  buf) == 0 && buf->len > 0) {
        yyjson_doc *doc = yyjson_read(buf->buf, buf->len, 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            yyjson_val *btc = yyjson_obj_get(root, "bitcoin");
            if (btc) {
                yyjson_val *usd = yyjson_obj_get(btc, "usd");
                if (usd) f->data.btc_price = get_json_number(usd);
            }
            yyjson_val *eth = yyjson_obj_get(root, "ethereum");
            if (eth) {
                yyjson_val *eth_btc = yyjson_obj_get(eth, "btc");
                if (eth_btc) f->data.eth_btc = get_json_number(eth_btc);
            }
            yyjson_doc_free(doc);
        }
    }
    return 0;
}

/* ── TradFi: Gold, S&P500, DXY ─────────────────────────────────────────── */
static int fetch_tradfi(tb_macro_fetcher_t *f, tb_curl_buf_t *buf) {
    /* Gold price via CoinGecko (tether-gold as proxy) */
    if (fetch_url(f->curl,
                  "https://api.coingecko.com/api/v3/simple/price?ids=tether-gold&vs_currencies=usd",
                  buf) == 0 && buf->len > 0) {
        yyjson_doc *doc = yyjson_read(buf->buf, buf->len, 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            yyjson_val *xaut = yyjson_obj_get(root, "tether-gold");
            if (xaut) {
                yyjson_val *usd = yyjson_obj_get(xaut, "usd");
                if (usd) f->data.gold = get_json_number(usd);
            }
            yyjson_doc_free(doc);
        }
    }

    /* S&P500 and DXY — requires API key (financialmodelingprep.com free tier) */
    if (f->api_key[0] != '\0') {
        char url[512];
        snprintf(url, sizeof(url),
                 "https://financialmodelingprep.com/api/v3/quote/%%5EGSPC,DX-Y.NYB?apikey=%s",
                 f->api_key);
        if (fetch_url(f->curl, url, buf) == 0 && buf->len > 0) {
            yyjson_doc *doc = yyjson_read(buf->buf, buf->len, 0);
            if (doc) {
                yyjson_val *root = yyjson_doc_get_root(doc);
                if (yyjson_is_arr(root)) {
                    size_t idx, max;
                    yyjson_val *item;
                    yyjson_arr_foreach(root, idx, max, item) {
                        yyjson_val *sym = yyjson_obj_get(item, "symbol");
                        yyjson_val *price = yyjson_obj_get(item, "price");
                        if (sym && price) {
                            const char *s = yyjson_get_str(sym);
                            if (s && strcmp(s, "^GSPC") == 0)
                                f->data.sp500 = get_json_number(price);
                            else if (s && strcmp(s, "DX-Y.NYB") == 0)
                                f->data.dxy = get_json_number(price);
                        }
                    }
                }
                yyjson_doc_free(doc);
            }
        }
    }

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_macro_fetcher_t *tb_macro_fetcher_create(const char *api_key) {
    tb_macro_fetcher_t *f = calloc(1, sizeof(tb_macro_fetcher_t));
    if (!f) return NULL;

    if (api_key && api_key[0] != '\0') {
        snprintf(f->api_key, sizeof(f->api_key), "%s", api_key);
    }

    f->curl = curl_easy_init();
    if (!f->curl) {
        free(f);
        return NULL;
    }

    pthread_mutex_init(&f->lock, NULL);
    tb_log_info("macro: fetcher created (api_key=%s)",
                f->api_key[0] ? "set" : "none");
    return f;
}

void tb_macro_fetcher_destroy(tb_macro_fetcher_t *f) {
    if (!f) return;
    if (f->curl) curl_easy_cleanup(f->curl);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

void tb_macro_fetcher_set_hl_rest(tb_macro_fetcher_t *f, hl_rest_t *rest) {
    f->hl_rest = rest;
    tb_log_info("macro: using Hyperliquid for crypto prices");
}

int tb_macro_fetcher_refresh(tb_macro_fetcher_t *f) {
    tb_curl_buf_t buf = { .buf = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.buf) return -1;

    tb_macro_data_t new_data;
    pthread_mutex_lock(&f->lock);
    new_data = f->data;  /* preserve old values as fallback */
    pthread_mutex_unlock(&f->lock);

    /* Work on local copy via struct field */
    f->data = new_data;

    /* 1. Crypto prices: prefer Hyperliquid (real-time, free) */
    if (fetch_hl_crypto(f) != 0) {
        /* Fallback to CoinGecko for BTC/ETH prices */
        fetch_coingecko_prices(f, &buf);
    }

    /* 2. CoinGecko for macro-only data (dominance, total2) */
    fetch_coingecko_macro(f, &buf);

    /* 3. TradFi: Gold, S&P500, DXY */
    fetch_tradfi(f, &buf);

    f->data.last_update_ms = (int64_t)time(NULL) * 1000;
    f->data.valid = true;

    /* Commit */
    pthread_mutex_lock(&f->lock);
    new_data = f->data;
    pthread_mutex_unlock(&f->lock);

    free(buf.buf);

    const char *src = f->hl_rest ? "HL" : "CoinGecko";
    tb_log_info("macro: BTC=$%.0f(%s) dom=%.1f%% ETH/BTC=%.5f Gold=$%.0f SP500=$%.0f DXY=%.1f T2=$%.0fB",
                new_data.btc_price, src, new_data.btc_dominance, new_data.eth_btc,
                new_data.gold, new_data.sp500, new_data.dxy, new_data.total2_mcap);
    return 0;
}

tb_macro_data_t tb_macro_fetcher_get(const tb_macro_fetcher_t *f) {
    pthread_mutex_lock((pthread_mutex_t *)&f->lock);
    tb_macro_data_t copy = f->data;
    pthread_mutex_unlock((pthread_mutex_t *)&f->lock);
    return copy;
}

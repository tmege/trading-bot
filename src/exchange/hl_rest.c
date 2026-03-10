#include "exchange/hl_rest.h"
#include "exchange/hl_json.h"
#include "exchange/hl_types.h"
#include "exchange/hl_signing.h"
#include "core/logging.h"
#include <curl/curl.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

/* ── Rate limiter ──────────────────────────────────────────────────────────── */
typedef struct {
    int          tokens;
    int          max_tokens;
    int64_t      last_refill_ms;
    pthread_mutex_t lock;
} rate_limiter_t;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void rate_limiter_init(rate_limiter_t *rl, int max_per_min) {
    rl->tokens = max_per_min;
    rl->max_tokens = max_per_min;
    rl->last_refill_ms = now_ms();
    pthread_mutex_init(&rl->lock, NULL);
}

static bool rate_limiter_acquire(rate_limiter_t *rl, int weight) {
    pthread_mutex_lock(&rl->lock);
    int64_t now = now_ms();
    int64_t elapsed = now - rl->last_refill_ms;
    if (elapsed >= 60000) {
        rl->tokens = rl->max_tokens;
        rl->last_refill_ms = now;
    } else {
        int refill = (int)(elapsed * rl->max_tokens / 60000);
        rl->tokens += refill;
        if (rl->tokens > rl->max_tokens) rl->tokens = rl->max_tokens;
        rl->last_refill_ms = now;
    }

    if (rl->tokens >= weight) {
        rl->tokens -= weight;
        pthread_mutex_unlock(&rl->lock);
        return true;
    }
    pthread_mutex_unlock(&rl->lock);
    return false;
}

/* ── Curl response buffer ──────────────────────────────────────────────────── */
typedef struct {
    char   *data;
    size_t  size;
} response_buf_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
    /* Guard against size_t overflow */
    if (nmemb > 0 && size > SIZE_MAX / nmemb) return 0;
    size_t total = size * nmemb;
    response_buf_t *buf = (response_buf_t *)userp;
    /* Cap at 16 MB to prevent unbounded growth */
    if (buf->size + total > 16 * 1024 * 1024) return 0;
    char *new_data = realloc(buf->data, buf->size + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* ── REST client struct ────────────────────────────────────────────────────── */
struct hl_rest {
    char            base_url[256];
    hl_signer_t    *signer;
    bool            is_mainnet;
    CURL           *curl;
    rate_limiter_t  rate_limiter;
    pthread_mutex_t curl_lock; /* curl is not thread-safe */
};

hl_rest_t *hl_rest_create(const char *base_url, hl_signer_t *signer,
                           bool is_mainnet) {
    hl_rest_t *r = calloc(1, sizeof(hl_rest_t));
    if (!r) return NULL;

    snprintf(r->base_url, sizeof(r->base_url), "%s", base_url);
    r->signer = signer;
    r->is_mainnet = is_mainnet;

    r->curl = curl_easy_init();
    if (!r->curl) {
        free(r);
        return NULL;
    }

    rate_limiter_init(&r->rate_limiter, 1200);
    pthread_mutex_init(&r->curl_lock, NULL);

    return r;
}

void hl_rest_destroy(hl_rest_t *rest) {
    if (!rest) return;
    if (rest->curl) curl_easy_cleanup(rest->curl);
    pthread_mutex_destroy(&rest->curl_lock);
    pthread_mutex_destroy(&rest->rate_limiter.lock);
    free(rest);
}

/* ── Internal POST helper ──────────────────────────────────────────────────── */
static int post_json(hl_rest_t *rest, const char *endpoint,
                     const char *body, int weight,
                     response_buf_t *resp) {
    if (!rate_limiter_acquire(&rest->rate_limiter, weight)) {
        tb_log_warn("rate limit exceeded, waiting...");
        struct timespec ts = {0, 100000000}; /* 100ms */
        nanosleep(&ts, NULL);
        if (!rate_limiter_acquire(&rest->rate_limiter, weight)) {
            tb_log_error("rate limit still exceeded after wait");
            return -1;
        }
    }

    char url[512];
    snprintf(url, sizeof(url), "%s%s", rest->base_url, endpoint);

    resp->data = NULL;
    resp->size = 0;

    pthread_mutex_lock(&rest->curl_lock);

    curl_easy_reset(rest->curl);
    curl_easy_setopt(rest->curl, CURLOPT_URL, url);
    curl_easy_setopt(rest->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(rest->curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(rest->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(rest->curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(rest->curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(rest->curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(rest->curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(rest->curl, CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(rest->curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(rest->curl);
    long http_code = 0;
    curl_easy_getinfo(rest->curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    pthread_mutex_unlock(&rest->curl_lock);

    if (res != CURLE_OK) {
        tb_log_error("curl error: %s (url=%s)", curl_easy_strerror(res), url);
        free(resp->data);
        resp->data = NULL;
        return -1;
    }

    if (http_code != 200) {
        tb_log_error("HTTP %ld: %s", http_code, resp->data ? resp->data : "(null)");
        free(resp->data);
        resp->data = NULL;
        return -1;
    }

    return 0;
}

/* ── Info helper: POST /info with type ─────────────────────────────────────── */
static yyjson_doc *info_request(hl_rest_t *rest, const char *body, int weight) {
    response_buf_t resp = {0};
    if (post_json(rest, "/info", body, weight, &resp) != 0) {
        return NULL;
    }

    yyjson_doc *doc = yyjson_read(resp.data, resp.size, 0);
    free(resp.data);

    if (!doc) {
        tb_log_error("JSON parse error on info response");
    }
    return doc;
}

/* ── Info endpoints ────────────────────────────────────────────────────────── */

int hl_rest_get_meta(hl_rest_t *rest, tb_asset_meta_t *out_assets, int *out_count) {
    yyjson_doc *doc = info_request(rest, "{\"type\":\"metaAndAssetCtxs\"}", 20);
    if (!doc) return -1;

    int rc = hl_json_parse_meta(yyjson_doc_get_root(doc), out_assets, out_count);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_all_mids(hl_rest_t *rest, tb_mid_t *out_mids, int *out_count) {
    yyjson_doc *doc = info_request(rest, "{\"type\":\"allMids\"}", 2);
    if (!doc) return -1;

    int rc = hl_json_parse_all_mids(yyjson_doc_get_root(doc), out_mids, out_count);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_l2_book(hl_rest_t *rest, const char *coin, tb_book_t *out_book) {
    char body[128];
    snprintf(body, sizeof(body), "{\"type\":\"l2Book\",\"coin\":\"%s\"}", coin);

    yyjson_doc *doc = info_request(rest, body, 2);
    if (!doc) return -1;

    int rc = hl_json_parse_l2_book(yyjson_doc_get_root(doc), out_book);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_candles(hl_rest_t *rest, const char *coin, const char *interval,
                        int64_t start_ms, int64_t end_ms,
                        tb_candle_t *out_candles, int *out_count,
                        int max_count) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"type\":\"candleSnapshot\",\"req\":{\"coin\":\"%s\","
             "\"interval\":\"%s\",\"startTime\":%lld,\"endTime\":%lld}}",
             coin, interval, (long long)start_ms, (long long)end_ms);

    yyjson_doc *doc = info_request(rest, body, 20);
    if (!doc) return -1;

    int rc = hl_json_parse_candles(yyjson_doc_get_root(doc), out_candles, out_count,
                                    max_count);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_account(hl_rest_t *rest, const char *user_addr, tb_account_t *out) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"type\":\"clearinghouseState\",\"user\":\"%s\"}", user_addr);

    yyjson_doc *doc = info_request(rest, body, 2);
    if (!doc) return -1;

    int rc = hl_json_parse_account(yyjson_doc_get_root(doc), out);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_open_orders(hl_rest_t *rest, const char *user_addr,
                            tb_order_t *out_orders, int *out_count,
                            int max_count) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"type\":\"openOrders\",\"user\":\"%s\"}", user_addr);

    yyjson_doc *doc = info_request(rest, body, 20);
    if (!doc) return -1;

    int rc = hl_json_parse_orders(yyjson_doc_get_root(doc), out_orders, out_count,
                                   max_count);
    yyjson_doc_free(doc);
    return rc;
}

int hl_rest_get_user_fills(hl_rest_t *rest, const char *user_addr,
                           tb_fill_t *out_fills, int *out_count,
                           int max_count) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"type\":\"userFills\",\"user\":\"%s\"}", user_addr);

    yyjson_doc *doc = info_request(rest, body, 20);
    if (!doc) return -1;

    int rc = hl_json_parse_fills(yyjson_doc_get_root(doc), out_fills, out_count,
                                  max_count);
    yyjson_doc_free(doc);
    return rc;
}

/* ── Exchange helper ───────────────────────────────────────────────────────── */

static int exchange_request(hl_rest_t *rest,
                            const uint8_t *msgpack_data, size_t msgpack_len,
                            const char *action_json,
                            yyjson_doc **out_doc) {
    /* Monotonic nonce: timestamp + atomic counter to prevent replay */
    static _Atomic uint64_t g_nonce_counter = 0;
    uint64_t nonce = (uint64_t)now_ms() * 1000 +
                     atomic_fetch_add(&g_nonce_counter, 1) % 1000;

    /* Sign */
    hl_signature_t sig;
    if (hl_sign_l1_action(rest->signer, msgpack_data, msgpack_len,
                           nonce, NULL, rest->is_mainnet, &sig) != 0) {
        tb_log_error("failed to sign action");
        return -1;
    }

    char sig_hex[134];
    hl_signature_to_hex(&sig, sig_hex, sizeof(sig_hex));

    /* Build request body */
    char *body = NULL;
    if (hl_json_build_exchange_body(action_json, nonce, sig_hex, NULL,
                                     &body) != 0) {
        tb_log_error("failed to build exchange body");
        return -1;
    }

    response_buf_t resp = {0};
    int rc = post_json(rest, "/exchange", body, 1, &resp);
    free(body);

    if (rc != 0) return -1;

    *out_doc = yyjson_read(resp.data, resp.size, 0);
    free(resp.data);

    if (!*out_doc) {
        tb_log_error("JSON parse error on exchange response");
        return -1;
    }

    return 0;
}

/* ── Exchange endpoints ────────────────────────────────────────────────────── */

int hl_rest_place_order(hl_rest_t *rest, const tb_order_request_t *order,
                        tb_grouping_t grouping, uint64_t *out_oid) {
    return hl_rest_place_orders(rest, order, 1, grouping, out_oid, NULL);
}

int hl_rest_place_orders(hl_rest_t *rest,
                         const tb_order_request_t *orders, int n_orders,
                         tb_grouping_t grouping,
                         uint64_t *out_oids, int *out_n_filled) {
    /* Serialize to msgpack for signing */
    uint8_t *mp_data = NULL;
    size_t mp_len = 0;
    if (hl_msgpack_order_action(orders, n_orders, grouping,
                                 &mp_data, &mp_len) != 0) {
        return -1;
    }

    /* Build action JSON for request body */
    yyjson_mut_doc *adoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *aroot = yyjson_mut_obj(adoc);
    yyjson_mut_doc_set_root(adoc, aroot);

    yyjson_mut_obj_add_str(adoc, aroot, "type", "order");

    yyjson_mut_val *arr = yyjson_mut_arr(adoc);
    for (int i = 0; i < n_orders; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(adoc);

        yyjson_mut_obj_add_uint(adoc, o, "a", orders[i].asset);
        yyjson_mut_obj_add_bool(adoc, o, "b", orders[i].side == TB_SIDE_BUY);

        char px_buf[64], sz_buf[64];
        tb_decimal_to_str(orders[i].price, px_buf, sizeof(px_buf));
        tb_decimal_to_str(orders[i].size, sz_buf, sizeof(sz_buf));
        yyjson_mut_obj_add_strcpy(adoc, o, "p", px_buf);
        yyjson_mut_obj_add_strcpy(adoc, o, "s", sz_buf);

        yyjson_mut_obj_add_bool(adoc, o, "r", orders[i].reduce_only);

        /* Type object */
        yyjson_mut_val *t = yyjson_mut_obj(adoc);
        if (orders[i].type == TB_ORDER_LIMIT) {
            yyjson_mut_val *lim = yyjson_mut_obj(adoc);
            const char *tif = "Gtc";
            if (orders[i].tif == TB_TIF_IOC) tif = "Ioc";
            else if (orders[i].tif == TB_TIF_ALO) tif = "Alo";
            yyjson_mut_obj_add_str(adoc, lim, "tif", tif);
            yyjson_mut_obj_add_val(adoc, t, "limit", lim);
        } else {
            yyjson_mut_val *trig = yyjson_mut_obj(adoc);
            yyjson_mut_obj_add_bool(adoc, trig, "isMarket", orders[i].is_market);
            char tpx_buf[64];
            tb_decimal_to_str(orders[i].trigger_px, tpx_buf, sizeof(tpx_buf));
            yyjson_mut_obj_add_strcpy(adoc, trig, "triggerPx", tpx_buf);
            yyjson_mut_obj_add_str(adoc, trig, "tpsl",
                                    orders[i].tpsl == TB_TPSL_TP ? "tp" : "sl");
            yyjson_mut_obj_add_val(adoc, t, "trigger", trig);
        }
        yyjson_mut_obj_add_val(adoc, o, "t", t);

        if (orders[i].cloid[0]) {
            yyjson_mut_obj_add_strcpy(adoc, o, "c", orders[i].cloid);
        }

        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_obj_add_val(adoc, aroot, "orders", arr);

    const char *grp_str = "na";
    if (grouping == TB_GROUP_NORMAL_TPSL) grp_str = "normalTpsl";
    else if (grouping == TB_GROUP_POS_TPSL) grp_str = "positionTpsl";
    yyjson_mut_obj_add_str(adoc, aroot, "grouping", grp_str);

    char *action_json = yyjson_mut_write(adoc, 0, NULL);
    yyjson_mut_doc_free(adoc);

    /* Send request */
    yyjson_doc *resp_doc = NULL;
    int rc = exchange_request(rest, mp_data, mp_len, action_json, &resp_doc);
    free(mp_data);
    free(action_json);

    if (rc != 0) return -1;

    char err_msg[256] = {0};
    rc = hl_json_parse_exchange_response(yyjson_doc_get_root(resp_doc),
                                          out_oids, n_orders, out_n_filled,
                                          err_msg, sizeof(err_msg));
    yyjson_doc_free(resp_doc);

    if (rc != 0) {
        tb_log_error("order placement failed: %s", err_msg);
    }
    return rc;
}

int hl_rest_cancel_order(hl_rest_t *rest, uint32_t asset, uint64_t oid) {
    return hl_rest_cancel_orders(rest, &asset, &oid, 1);
}

int hl_rest_cancel_orders(hl_rest_t *rest,
                          const uint32_t *assets, const uint64_t *oids,
                          int n_cancels) {
    uint8_t *mp_data = NULL;
    size_t mp_len = 0;
    if (hl_msgpack_cancel_action(assets, oids, n_cancels,
                                  &mp_data, &mp_len) != 0) {
        return -1;
    }

    /* Build action JSON */
    yyjson_mut_doc *adoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *aroot = yyjson_mut_obj(adoc);
    yyjson_mut_doc_set_root(adoc, aroot);

    yyjson_mut_obj_add_str(adoc, aroot, "type", "cancel");

    yyjson_mut_val *arr = yyjson_mut_arr(adoc);
    for (int i = 0; i < n_cancels; i++) {
        yyjson_mut_val *c = yyjson_mut_obj(adoc);
        yyjson_mut_obj_add_uint(adoc, c, "a", assets[i]);
        yyjson_mut_obj_add_uint(adoc, c, "o", oids[i]);
        yyjson_mut_arr_append(arr, c);
    }
    yyjson_mut_obj_add_val(adoc, aroot, "cancels", arr);

    char *action_json = yyjson_mut_write(adoc, 0, NULL);
    yyjson_mut_doc_free(adoc);

    yyjson_doc *resp_doc = NULL;
    int rc = exchange_request(rest, mp_data, mp_len, action_json, &resp_doc);
    free(mp_data);
    free(action_json);

    if (rc != 0) return -1;

    char err_msg[256] = {0};
    rc = hl_json_parse_exchange_response(yyjson_doc_get_root(resp_doc),
                                          NULL, 0, NULL, err_msg, sizeof(err_msg));
    yyjson_doc_free(resp_doc);

    if (rc != 0) {
        tb_log_error("cancel failed: %s", err_msg);
    }
    return rc;
}

int hl_rest_update_leverage(hl_rest_t *rest,
                            uint32_t asset, int leverage, bool is_cross) {
    uint8_t *mp_data = NULL;
    size_t mp_len = 0;
    if (hl_msgpack_update_leverage_action(asset, is_cross, leverage,
                                           &mp_data, &mp_len) != 0) {
        return -1;
    }

    char action_json[256];
    snprintf(action_json, sizeof(action_json),
             "{\"type\":\"updateLeverage\",\"asset\":%u,\"isCross\":%s,\"leverage\":%d}",
             asset, is_cross ? "true" : "false", leverage);

    yyjson_doc *resp_doc = NULL;
    int rc = exchange_request(rest, mp_data, mp_len, action_json, &resp_doc);
    free(mp_data);

    if (rc != 0) return -1;

    char err_msg[256] = {0};
    rc = hl_json_parse_exchange_response(yyjson_doc_get_root(resp_doc),
                                          NULL, 0, NULL, err_msg, sizeof(err_msg));
    yyjson_doc_free(resp_doc);

    if (rc != 0) {
        tb_log_error("update leverage failed: %s", err_msg);
    }
    return rc;
}

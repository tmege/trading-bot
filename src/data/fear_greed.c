#include "data/fear_greed.h"
#include "core/logging.h"

#include <curl/curl.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

struct tb_fear_greed_fetcher {
    CURL            *curl;
    tb_fear_greed_t  data;
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

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_fear_greed_fetcher_t *tb_fear_greed_create(void) {
    tb_fear_greed_fetcher_t *f = calloc(1, sizeof(tb_fear_greed_fetcher_t));
    if (!f) return NULL;

    f->curl = curl_easy_init();
    if (!f->curl) {
        free(f);
        return NULL;
    }

    pthread_mutex_init(&f->lock, NULL);
    tb_log_info("fear_greed: fetcher created");
    return f;
}

void tb_fear_greed_destroy(tb_fear_greed_fetcher_t *f) {
    if (!f) return;
    if (f->curl) curl_easy_cleanup(f->curl);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

int tb_fear_greed_refresh(tb_fear_greed_fetcher_t *f) {
    tb_curl_buf_t buf = { .buf = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.buf) return -1;

    buf.len = 0;
    curl_easy_setopt(f->curl, CURLOPT_URL, "https://api.alternative.me/fng/?limit=1");
    curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(f->curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(f->curl, CURLOPT_USERAGENT, "trading-bot/1.0");
    curl_easy_setopt(f->curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(f->curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(f->curl);
    if (res != CURLE_OK) {
        tb_log_warn("fear_greed: fetch failed: %s", curl_easy_strerror(res));
        free(buf.buf);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        tb_log_warn("fear_greed: HTTP %ld", http_code);
        free(buf.buf);
        return -1;
    }

    /* Parse: {"name":"Fear and Greed Index","data":[{"value":"25","value_classification":"Extreme Fear","timestamp":"..."}]} */
    tb_fear_greed_t new_data;
    memset(&new_data, 0, sizeof(new_data));

    yyjson_doc *doc = yyjson_read(buf.buf, buf.len, 0);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *data_arr = yyjson_obj_get(root, "data");
        if (data_arr && yyjson_is_arr(data_arr)) {
            yyjson_val *item = yyjson_arr_get_first(data_arr);
            if (item) {
                yyjson_val *val = yyjson_obj_get(item, "value");
                if (val) {
                    const char *vs = yyjson_get_str(val);
                    if (vs) new_data.value = atoi(vs);
                }

                yyjson_val *cls = yyjson_obj_get(item, "value_classification");
                if (cls) {
                    const char *cs = yyjson_get_str(cls);
                    if (cs) snprintf(new_data.label, sizeof(new_data.label), "%s", cs);
                }

                yyjson_val *ts = yyjson_obj_get(item, "timestamp");
                if (ts) {
                    const char *tss = yyjson_get_str(ts);
                    if (tss) new_data.timestamp = atoll(tss);
                }
            }
        }
        yyjson_doc_free(doc);
    }

    new_data.last_update_ms = (int64_t)time(NULL) * 1000;
    new_data.valid = (new_data.value > 0 || new_data.label[0] != '\0');

    /* Commit */
    pthread_mutex_lock(&f->lock);
    f->data = new_data;
    pthread_mutex_unlock(&f->lock);

    free(buf.buf);

    tb_log_info("fear_greed: %d — %s", new_data.value, new_data.label);
    return 0;
}

tb_fear_greed_t tb_fear_greed_get(const tb_fear_greed_fetcher_t *f) {
    pthread_mutex_lock((pthread_mutex_t *)&f->lock);
    tb_fear_greed_t copy = f->data;
    pthread_mutex_unlock((pthread_mutex_t *)&f->lock);
    return copy;
}

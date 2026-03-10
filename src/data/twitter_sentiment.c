#include "data/twitter_sentiment.h"
#include "core/logging.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* ── Keyword scoring tables ─────────────────────────────────────────────── */
typedef struct {
    const char *word;
    double      weight;
} tb_keyword_t;

static const tb_keyword_t BULLISH_KEYWORDS[] = {
    {"moon",     0.8}, {"pump",     0.7}, {"bull",     0.7}, {"bullish",  0.9},
    {"buy",      0.5}, {"long",     0.5}, {"rocket",   0.7}, {"ath",      0.8},
    {"breakout", 0.7}, {"surge",    0.6}, {"rally",    0.7}, {"green",    0.4},
    {"higher",   0.4}, {"up",       0.3}, {"gain",     0.5}, {"profit",   0.5},
    {"launch",   0.5}, {"massive",  0.4}, {"boom",     0.6}, {"doge",     0.3},
    {"bitcoin",  0.1}, {"crypto",   0.1}, {"adoption", 0.5}, {"bullrun",  0.9},
    {"lambo",    0.6}, {"wagmi",    0.7}, {"hodl",     0.5}, {"diamond",  0.5},
    {"accumulate", 0.6}, {"loading", 0.5}, {"undervalued", 0.6},
    {NULL, 0}
};

static const tb_keyword_t BEARISH_KEYWORDS[] = {
    {"crash",   -0.8}, {"dump",    -0.7}, {"bear",    -0.7}, {"bearish", -0.9},
    {"sell",    -0.5}, {"short",   -0.5}, {"rekt",    -0.7}, {"scam",    -0.8},
    {"rug",     -0.9}, {"fear",    -0.5}, {"panic",   -0.6}, {"red",     -0.4},
    {"lower",   -0.4}, {"down",    -0.3}, {"loss",    -0.5}, {"dead",    -0.6},
    {"bubble",  -0.5}, {"fraud",   -0.7}, {"ban",     -0.6}, {"sec",     -0.4},
    {"warning", -0.5}, {"collapse",-0.8}, {"liquidat",-0.7}, {"ngmi",    -0.7},
    {"overvalued", -0.6}, {"top",  -0.3}, {"exit",    -0.4},
    {NULL, 0}
};

/* ── RSS feed sources (priority order) ─────────────────────────────────── */
typedef struct {
    const char *url;
    const char *name;
} tb_rss_source_t;

static const tb_rss_source_t RSS_SOURCES[] = {
    /* Primary: CryptoPanic (public RSS, no auth needed) */
    { "https://cryptopanic.com/news/rss/",             "CryptoPanic"   },
    /* Secondary: CoinDesk RSS */
    { "https://www.coindesk.com/arc/outboundfeeds/rss/", "CoinDesk"    },
    /* Tertiary: Cointelegraph RSS */
    { "https://cointelegraph.com/rss",                   "Cointelegraph"},
    /* Quaternary: Decrypt RSS */
    { "https://decrypt.co/feed",                         "Decrypt"      },
    { NULL, NULL }
};

struct tb_twitter_sentiment {
    char                 accounts[TB_MAX_TWITTER_ACCOUNTS][64];
    int                  n_accounts;
    CURL                *curl;
    tb_sentiment_data_t  data;
    pthread_mutex_t      lock;
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

/* ── Score a single text ───────────────────────────────────────────────── */
static double score_text(const char *text) {
    double score = 0.0;
    int hits = 0;

    /* Convert to lowercase for matching */
    size_t len = strlen(text);
    char *lower = malloc(len + 1);
    if (!lower) return 0.0;
    for (size_t i = 0; i <= len; i++) {
        lower[i] = (char)tolower((unsigned char)text[i]);
    }

    for (int i = 0; BULLISH_KEYWORDS[i].word; i++) {
        if (strstr(lower, BULLISH_KEYWORDS[i].word)) {
            score += BULLISH_KEYWORDS[i].weight;
            hits++;
        }
    }

    for (int i = 0; BEARISH_KEYWORDS[i].word; i++) {
        if (strstr(lower, BEARISH_KEYWORDS[i].word)) {
            score += BEARISH_KEYWORDS[i].weight;
            hits++;
        }
    }

    free(lower);

    if (hits == 0) return 0.0;

    /* Normalize to -1..+1 */
    double normalized = score / (double)hits;
    if (normalized > 1.0) normalized = 1.0;
    if (normalized < -1.0) normalized = -1.0;
    return normalized;
}

/* ── Extract items from RSS/Atom XML ────────────────────────────────────── */
static int parse_rss_items(const char *xml, const char *source,
                            tb_tweet_t *out, int max_items) {
    int count = 0;
    const char *p = xml;

    /* Simple XML parsing: look for <item><title>...<description>... */
    while (count < max_items) {
        const char *item = strstr(p, "<item>");
        if (!item) {
            /* Also try Atom format */
            item = strstr(p, "<entry>");
            if (!item) break;
        }

        const char *title_start = strstr(item, "<title>");
        const char *title_end = title_start ? strstr(title_start, "</title>") : NULL;

        const char *desc_start = strstr(item, "<description>");
        if (!desc_start) desc_start = strstr(item, "<content");
        const char *desc_end = desc_start ? strstr(desc_start, "</description>") : NULL;
        if (!desc_end) desc_end = desc_start ? strstr(desc_start, "</content>") : NULL;

        /* Use title or description as text */
        const char *text_start = NULL;
        const char *text_end = NULL;

        if (title_start && title_end) {
            text_start = title_start + 7; /* len("<title>") */
            text_end = title_end;
        } else if (desc_start && desc_end) {
            /* Skip the tag itself */
            text_start = strchr(desc_start, '>');
            if (text_start) text_start++;
            text_end = desc_end;
        }

        if (text_start && text_end && text_end > text_start) {
            size_t tlen = (size_t)(text_end - text_start);
            if (tlen >= sizeof(out[count].text)) tlen = sizeof(out[count].text) - 1;
            memcpy(out[count].text, text_start, tlen);
            out[count].text[tlen] = '\0';
            snprintf(out[count].account, sizeof(out[count].account), "%s", source);
            out[count].timestamp_ms = (int64_t)time(NULL) * 1000;
            out[count].score = score_text(out[count].text);
            count++;
        }

        p = item + 6;
    }

    return count;
}

/* ── Fetch from an RSS URL ──────────────────────────────────────────────── */
static int fetch_rss(CURL *curl, const char *url, const char *source_name,
                      tb_tweet_t *out, int max_items) {
    tb_curl_buf_t buf = { .buf = malloc(65536), .len = 0, .cap = 65536 };
    if (!buf.buf) return 0;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "TradingBot/1.0 (sentiment analyzer)");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    int result = 0;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200 && buf.len > 0) {
            result = parse_rss_items(buf.buf, source_name, out, max_items);
            if (result > 0) {
                tb_log_debug("sentiment: fetched %d items from %s", result, source_name);
            }
        } else {
            tb_log_debug("sentiment: %s returned HTTP %ld", source_name, http_code);
        }
    } else {
        tb_log_debug("sentiment: %s fetch failed: %s", source_name,
                     curl_easy_strerror(res));
    }

    free(buf.buf);
    return result;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_twitter_sentiment_t *tb_sentiment_create(const char **accounts, int n_accounts) {
    tb_twitter_sentiment_t *s = calloc(1, sizeof(tb_twitter_sentiment_t));
    if (!s) return NULL;

    s->n_accounts = n_accounts < TB_MAX_TWITTER_ACCOUNTS ?
                    n_accounts : TB_MAX_TWITTER_ACCOUNTS;
    for (int i = 0; i < s->n_accounts; i++) {
        snprintf(s->accounts[i], sizeof(s->accounts[i]), "%s", accounts[i]);
    }

    s->curl = curl_easy_init();
    if (!s->curl) {
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lock, NULL);

    tb_log_info("sentiment: initialized with %d RSS sources",
                (int)(sizeof(RSS_SOURCES) / sizeof(RSS_SOURCES[0]) - 1));

    return s;
}

void tb_sentiment_destroy(tb_twitter_sentiment_t *s) {
    if (!s) return;
    if (s->curl) curl_easy_cleanup(s->curl);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

int tb_sentiment_refresh(tb_twitter_sentiment_t *s) {
    tb_tweet_t *all_items = malloc(sizeof(tb_tweet_t) * TB_MAX_TWEETS_STORED);
    if (!all_items) return -1;
    int total_items = 0;

    /* Fetch from RSS sources in priority order (stop when we have enough) */
    for (int i = 0; RSS_SOURCES[i].url && total_items < 10; i++) {
        int remaining = TB_MAX_TWEETS_STORED - total_items;
        int n = fetch_rss(s->curl, RSS_SOURCES[i].url, RSS_SOURCES[i].name,
                          &all_items[total_items], remaining > 20 ? 20 : remaining);
        total_items += n;
    }

    if (total_items == 0) {
        tb_log_warn("sentiment: no data from any RSS source");
    }

    /* Compute aggregate sentiment */
    double total_score = 0.0;
    int bullish = 0, bearish = 0;

    tb_sentiment_data_t new_data;
    memset(&new_data, 0, sizeof(new_data));

    for (int i = 0; i < total_items; i++) {
        total_score += all_items[i].score;
        if (all_items[i].score > 0.1) bullish++;
        else if (all_items[i].score < -0.1) bearish++;
    }

    new_data.total_tweets = total_items;
    if (total_items > 0) {
        new_data.overall_score = total_score / total_items;
        new_data.bullish_pct = (double)bullish / total_items * 100.0;
        new_data.bearish_pct = (double)bearish / total_items * 100.0;
    }

    /* Per-source aggregation */
    int src_count = 0;
    for (int i = 0; RSS_SOURCES[i].url; i++) {
        if (src_count >= TB_MAX_TWITTER_ACCOUNTS) break;
        snprintf(new_data.accounts[src_count].account,
                 sizeof(new_data.accounts[src_count].account),
                 "%s", RSS_SOURCES[i].name);
        double src_score = 0.0;
        int src_items = 0;
        for (int t = 0; t < total_items; t++) {
            if (strcmp(all_items[t].account, RSS_SOURCES[i].name) == 0) {
                src_score += all_items[t].score;
                src_items++;
            }
        }
        new_data.accounts[src_count].tweet_count = src_items;
        new_data.accounts[src_count].score = src_items > 0 ? src_score / src_items : 0.0;
        src_count++;
    }
    new_data.n_accounts = src_count;

    /* Copy recent items */
    new_data.n_recent = total_items;
    if (new_data.n_recent > TB_MAX_TWEETS_STORED)
        new_data.n_recent = TB_MAX_TWEETS_STORED;
    memcpy(new_data.recent, all_items, sizeof(tb_tweet_t) * (size_t)new_data.n_recent);

    new_data.last_update_ms = (int64_t)time(NULL) * 1000;
    new_data.valid = total_items > 0;

    /* Commit */
    pthread_mutex_lock(&s->lock);
    s->data = new_data;
    pthread_mutex_unlock(&s->lock);

    tb_log_debug("sentiment: %d items, score=%.2f (bull=%.0f%% bear=%.0f%%)",
                total_items, new_data.overall_score,
                new_data.bullish_pct, new_data.bearish_pct);
    free(all_items);
    return 0;
}

tb_sentiment_data_t tb_sentiment_get(const tb_twitter_sentiment_t *s) {
    pthread_mutex_lock((pthread_mutex_t *)&s->lock);
    tb_sentiment_data_t copy = s->data;
    pthread_mutex_unlock((pthread_mutex_t *)&s->lock);
    return copy;
}

bool tb_sentiment_has_spike(const tb_twitter_sentiment_t *s,
                            double threshold, int minutes) {
    pthread_mutex_lock((pthread_mutex_t *)&s->lock);
    int64_t cutoff = ((int64_t)time(NULL) - minutes * 60) * 1000;
    int spike_count = 0;
    double spike_score = 0.0;

    for (int i = 0; i < s->data.n_recent; i++) {
        if (s->data.recent[i].timestamp_ms >= cutoff) {
            spike_score += s->data.recent[i].score;
            spike_count++;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&s->lock);

    if (spike_count == 0) return false;
    double avg = spike_score / spike_count;
    return (avg > threshold || avg < -threshold);
}

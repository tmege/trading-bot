#include "data/data_manager.h"
#include "core/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* Refresh intervals in seconds */
#define MACRO_INTERVAL_SEC     300   /* 5 minutes */
#define SENTIMENT_INTERVAL_SEC 60    /* 1 minute */
#define FEAR_GREED_INTERVAL_SEC 3600 /* 1 hour (data updates daily anyway) */

/* Twitter accounts to follow */
static const char *DEFAULT_ACCOUNTS[] = {
    "eliz883",
    "WatcherGuru",
    "realDonaldTrump",
    "elonmusk",
};
#define N_DEFAULT_ACCOUNTS 4

struct tb_data_mgr {
    tb_macro_fetcher_t       *macro;
    tb_twitter_sentiment_t   *sentiment;
    tb_fear_greed_fetcher_t  *fear_greed;

    pthread_t    thread;
    _Atomic bool running;
    bool         started;

    /* Last refresh timestamps */
    int64_t     last_macro_ts;
    int64_t     last_sentiment_ts;
    int64_t     last_fg_ts;
};

static void *data_thread_func(void *arg) {
    tb_data_mgr_t *mgr = (tb_data_mgr_t *)arg;
    tb_log_info("data: background thread started");

    /* Initial fetch */
    if (mgr->macro) {
        tb_macro_fetcher_refresh(mgr->macro);
        mgr->last_macro_ts = (int64_t)time(NULL);
    }
    if (mgr->fear_greed) {
        tb_fear_greed_refresh(mgr->fear_greed);
        mgr->last_fg_ts = (int64_t)time(NULL);
    }
    if (mgr->sentiment) {
        tb_sentiment_refresh(mgr->sentiment);
        mgr->last_sentiment_ts = (int64_t)time(NULL);
    }

    while (mgr->running) {
        int64_t now = (int64_t)time(NULL);

        /* Macro refresh */
        if (mgr->macro && (now - mgr->last_macro_ts) >= MACRO_INTERVAL_SEC) {
            tb_macro_fetcher_refresh(mgr->macro);
            mgr->last_macro_ts = now;
        }

        /* Sentiment refresh */
        if (mgr->sentiment && (now - mgr->last_sentiment_ts) >= SENTIMENT_INTERVAL_SEC) {
            tb_sentiment_refresh(mgr->sentiment);
            mgr->last_sentiment_ts = now;
        }

        /* Fear & Greed refresh */
        if (mgr->fear_greed && (now - mgr->last_fg_ts) >= FEAR_GREED_INTERVAL_SEC) {
            tb_fear_greed_refresh(mgr->fear_greed);
            mgr->last_fg_ts = now;
        }

        /* Sleep 5s between checks */
        for (int i = 0; i < 50 && mgr->running; i++) {
            usleep(100000); /* 100ms */
        }
    }

    tb_log_info("data: background thread stopped");
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_data_mgr_t *tb_data_mgr_create(const tb_config_t *cfg) {
    tb_data_mgr_t *mgr = calloc(1, sizeof(tb_data_mgr_t));
    if (!mgr) return NULL;

    mgr->macro = tb_macro_fetcher_create(cfg->macro_api_key);
    mgr->sentiment = tb_sentiment_create(DEFAULT_ACCOUNTS, N_DEFAULT_ACCOUNTS);
    mgr->fear_greed = tb_fear_greed_create();

    if (!mgr->macro || !mgr->sentiment || !mgr->fear_greed) {
        tb_log_warn("data: some fetchers failed to create");
    }

    tb_log_info("data: manager created");
    return mgr;
}

void tb_data_mgr_destroy(tb_data_mgr_t *mgr) {
    if (!mgr) return;

    if (mgr->started) {
        tb_data_mgr_stop(mgr);
    }

    if (mgr->macro) tb_macro_fetcher_destroy(mgr->macro);
    if (mgr->sentiment) tb_sentiment_destroy(mgr->sentiment);
    if (mgr->fear_greed) tb_fear_greed_destroy(mgr->fear_greed);

    free(mgr);
}

int tb_data_mgr_start(tb_data_mgr_t *mgr) {
    if (mgr->started) return 0;

    mgr->running = true;
    if (pthread_create(&mgr->thread, NULL, data_thread_func, mgr) != 0) {
        tb_log_error("data: failed to start thread");
        mgr->running = false;
        return -1;
    }

    mgr->started = true;
    tb_log_info("data: manager started");
    return 0;
}

void tb_data_mgr_stop(tb_data_mgr_t *mgr) {
    if (!mgr->started) return;

    mgr->running = false;
    pthread_join(mgr->thread, NULL);
    mgr->started = false;
    tb_log_info("data: manager stopped");
}

tb_macro_data_t tb_data_mgr_get_macro(const tb_data_mgr_t *mgr) {
    if (mgr->macro) return tb_macro_fetcher_get(mgr->macro);
    tb_macro_data_t empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
}

tb_sentiment_data_t tb_data_mgr_get_sentiment(const tb_data_mgr_t *mgr) {
    if (mgr->sentiment) return tb_sentiment_get(mgr->sentiment);
    tb_sentiment_data_t empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
}

tb_fear_greed_t tb_data_mgr_get_fear_greed(const tb_data_mgr_t *mgr) {
    if (mgr->fear_greed) return tb_fear_greed_get(mgr->fear_greed);
    tb_fear_greed_t empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
}

int tb_data_mgr_refresh_all(tb_data_mgr_t *mgr) {
    int rc = 0;
    if (mgr->macro && tb_macro_fetcher_refresh(mgr->macro) != 0) rc = -1;
    if (mgr->sentiment && tb_sentiment_refresh(mgr->sentiment) != 0) rc = -1;
    if (mgr->fear_greed && tb_fear_greed_refresh(mgr->fear_greed) != 0) rc = -1;
    return rc;
}

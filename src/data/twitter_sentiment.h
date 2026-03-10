#ifndef TB_TWITTER_SENTIMENT_H
#define TB_TWITTER_SENTIMENT_H

#include <stdbool.h>
#include <stdint.h>

/* ── Sentiment score per account ────────────────────────────────────────── */
#define TB_MAX_TWITTER_ACCOUNTS 16
#define TB_MAX_TWEETS_STORED    64

typedef struct {
    char    text[512];
    int64_t timestamp_ms;
    double  score;           /* -1.0 (very bearish) to +1.0 (very bullish) */
    char    account[64];
} tb_tweet_t;

typedef struct {
    /* Aggregate sentiment */
    double   overall_score;      /* -1.0 to +1.0 */
    double   bullish_pct;        /* 0–100 */
    double   bearish_pct;        /* 0–100 */
    int      total_tweets;       /* tweets analyzed in window */

    /* Per-account sentiment */
    struct {
        char   account[64];
        double score;
        int    tweet_count;
    } accounts[TB_MAX_TWITTER_ACCOUNTS];
    int      n_accounts;

    /* Recent tweets */
    tb_tweet_t recent[TB_MAX_TWEETS_STORED];
    int        n_recent;

    /* Metadata */
    int64_t  last_update_ms;
    bool     valid;
} tb_sentiment_data_t;

typedef struct tb_twitter_sentiment tb_twitter_sentiment_t;

/* Create sentiment analyzer with list of accounts to follow */
tb_twitter_sentiment_t *tb_sentiment_create(const char **accounts, int n_accounts);
void                    tb_sentiment_destroy(tb_twitter_sentiment_t *s);

/* Refresh: fetch latest tweets and compute sentiment (blocking) */
int tb_sentiment_refresh(tb_twitter_sentiment_t *s);

/* Get latest snapshot (thread-safe) */
tb_sentiment_data_t tb_sentiment_get(const tb_twitter_sentiment_t *s);

/* Check if there's a recent spike (score > threshold within last N minutes) */
bool tb_sentiment_has_spike(const tb_twitter_sentiment_t *s,
                            double threshold, int minutes);

#endif /* TB_TWITTER_SENTIMENT_H */

#ifndef TB_FEAR_GREED_H
#define TB_FEAR_GREED_H

#include <stdbool.h>
#include <stdint.h>

/* ── Fear & Greed Index ─────────────────────────────────────────────────── */
typedef struct {
    int      value;             /* 0 (extreme fear) to 100 (extreme greed) */
    char     label[32];         /* "Extreme Fear", "Fear", "Neutral", "Greed", "Extreme Greed" */
    int64_t  timestamp;         /* Unix epoch */
    int64_t  last_update_ms;
    bool     valid;
} tb_fear_greed_t;

typedef struct tb_fear_greed_fetcher tb_fear_greed_fetcher_t;

tb_fear_greed_fetcher_t *tb_fear_greed_create(void);
void                     tb_fear_greed_destroy(tb_fear_greed_fetcher_t *f);

/* Fetch latest index (blocking). Returns 0 on success. */
int tb_fear_greed_refresh(tb_fear_greed_fetcher_t *f);

/* Get latest value (thread-safe). */
tb_fear_greed_t tb_fear_greed_get(const tb_fear_greed_fetcher_t *f);

#endif /* TB_FEAR_GREED_H */

#ifndef TB_DATA_MANAGER_H
#define TB_DATA_MANAGER_H

#include "data/macro_fetcher.h"
#include "data/twitter_sentiment.h"
#include "data/fear_greed.h"
#include "core/config.h"
#include <stdbool.h>

typedef struct tb_data_mgr tb_data_mgr_t;

/* Create data manager. Initializes all fetchers. */
tb_data_mgr_t *tb_data_mgr_create(const tb_config_t *cfg);
void            tb_data_mgr_destroy(tb_data_mgr_t *mgr);

/* Start background refresh thread. */
int  tb_data_mgr_start(tb_data_mgr_t *mgr);
void tb_data_mgr_stop(tb_data_mgr_t *mgr);

/* Get latest data snapshots (thread-safe). */
tb_macro_data_t     tb_data_mgr_get_macro(const tb_data_mgr_t *mgr);
tb_sentiment_data_t tb_data_mgr_get_sentiment(const tb_data_mgr_t *mgr);
tb_fear_greed_t     tb_data_mgr_get_fear_greed(const tb_data_mgr_t *mgr);

/* Force immediate refresh of all sources. */
int tb_data_mgr_refresh_all(tb_data_mgr_t *mgr);

#endif /* TB_DATA_MANAGER_H */

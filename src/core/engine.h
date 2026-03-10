#ifndef TB_ENGINE_H
#define TB_ENGINE_H

#include "core/config.h"
#include <stdbool.h>

typedef struct tb_engine tb_engine_t;

/* Create engine with loaded config. Returns NULL on failure. */
tb_engine_t *tb_engine_create(const tb_config_t *cfg);

/* Start all subsystem threads. Returns 0 on success. */
int tb_engine_start(tb_engine_t *engine);

/* Graceful shutdown: stops all threads, closes connections. */
void tb_engine_stop(tb_engine_t *engine);

/* Free all resources. */
void tb_engine_destroy(tb_engine_t *engine);

/* Check if engine is running */
bool tb_engine_is_running(const tb_engine_t *engine);

#endif /* TB_ENGINE_H */

#ifndef TB_ERROR_H
#define TB_ERROR_H

typedef enum {
    TB_OK = 0,
    TB_ERR_CONFIG,
    TB_ERR_NETWORK,
    TB_ERR_AUTH,
    TB_ERR_EXCHANGE,
    TB_ERR_SIGNING,
    TB_ERR_JSON,
    TB_ERR_WEBSOCKET,
    TB_ERR_LUA,
    TB_ERR_RISK,
    TB_ERR_DB,
    TB_ERR_OOM,
    TB_ERR_RATE_LIMIT,
    TB_ERR_INVALID_PARAM,
} tb_error_t;

const char *tb_error_str(tb_error_t err);

#endif /* TB_ERROR_H */

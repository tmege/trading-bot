#include "core/error.h"

static const char *error_strings[] = {
    [TB_OK]                = "success",
    [TB_ERR_CONFIG]        = "configuration error",
    [TB_ERR_NETWORK]       = "network error",
    [TB_ERR_AUTH]          = "authentication error",
    [TB_ERR_EXCHANGE]      = "exchange error",
    [TB_ERR_SIGNING]       = "signing error",
    [TB_ERR_JSON]          = "JSON parse error",
    [TB_ERR_WEBSOCKET]     = "WebSocket error",
    [TB_ERR_LUA]           = "Lua engine error",
    [TB_ERR_RISK]          = "risk check failed",
    [TB_ERR_DB]            = "database error",
    [TB_ERR_OOM]           = "out of memory",
    [TB_ERR_RATE_LIMIT]    = "rate limit exceeded",
    [TB_ERR_INVALID_PARAM] = "invalid parameter",
};

const char *tb_error_str(tb_error_t err) {
    if (err < 0 || err >= (int)(sizeof(error_strings) / sizeof(error_strings[0])))
        return "unknown error";
    return error_strings[err] ? error_strings[err] : "unknown error";
}

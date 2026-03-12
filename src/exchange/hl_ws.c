#include "exchange/hl_ws.h"
#include "exchange/hl_json.h"
#include "core/logging.h"
#include <libwebsockets.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

/* ── Message queue for outgoing messages ───────────────────────────────────── */
#define WS_QUEUE_SIZE 64
#define WS_MSG_MAX    4096

typedef struct {
    char    msgs[WS_QUEUE_SIZE][WS_MSG_MAX];
    int     head;
    int     tail;
    int     count;
    pthread_mutex_t lock;
} ws_queue_t;

static void ws_queue_init(ws_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
}

static bool ws_queue_push(ws_queue_t *q, const char *msg) {
    pthread_mutex_lock(&q->lock);
    if (q->count >= WS_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    snprintf(q->msgs[q->tail], WS_MSG_MAX, "%s", msg);
    q->tail = (q->tail + 1) % WS_QUEUE_SIZE;
    q->count++;
    pthread_mutex_unlock(&q->lock);
    return true;
}

static bool ws_queue_pop(ws_queue_t *q, char *out, size_t out_len) {
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    snprintf(out, out_len, "%s", q->msgs[q->head]);
    q->head = (q->head + 1) % WS_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

/* ── Saved subscriptions for reconnect ─────────────────────────────────────── */
#define MAX_SUBS 32

/* ── WebSocket client struct ───────────────────────────────────────────────── */
struct hl_ws {
    char                ws_url[256];
    char                ws_host[128];
    char                ws_path[128];
    int                 ws_port;
    bool                use_ssl;

    hl_ws_callbacks_t   cbs;
    struct lws_context *lws_ctx;
    struct lws         *wsi;

    pthread_t           thread;
    _Atomic bool        running;
    _Atomic bool        connected;

    ws_queue_t          send_queue;

    /* Receive buffer for fragmented messages */
    char               *recv_buf;
    size_t              recv_len;
    size_t              recv_cap;

    /* Reconnection state */
    int                 reconnect_attempts;
    int                 reconnect_delay_ms;

    /* Saved subscriptions for auto-resubscribe */
    char                subs[MAX_SUBS][WS_MSG_MAX];
    int                 n_subs;
    pthread_mutex_t     subs_lock;
};

/* ── URL parsing ───────────────────────────────────────────────────────────── */
static void parse_ws_url(hl_ws_t *ws) {
    const char *url = ws->ws_url;
    ws->use_ssl = (strncmp(url, "wss://", 6) == 0);
    const char *host_start = strstr(url, "://");
    if (host_start) host_start += 3;
    else host_start = url;

    const char *path_start = strchr(host_start, '/');
    if (path_start) {
        size_t host_len = (size_t)(path_start - host_start);
        if (host_len >= sizeof(ws->ws_host)) host_len = sizeof(ws->ws_host) - 1;
        memcpy(ws->ws_host, host_start, host_len);
        ws->ws_host[host_len] = '\0';
        snprintf(ws->ws_path, sizeof(ws->ws_path), "%s", path_start);
    } else {
        snprintf(ws->ws_host, sizeof(ws->ws_host), "%s", host_start);
        snprintf(ws->ws_path, sizeof(ws->ws_path), "/");
    }

    ws->ws_port = ws->use_ssl ? 443 : 80;
}

/* ── Message dispatch ──────────────────────────────────────────────────────── */
static void dispatch_message(hl_ws_t *ws, const char *data, size_t len) {
    yyjson_doc *doc = yyjson_read(data, len, 0);
    if (!doc) return;

    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *channel = NULL;

    yyjson_val *ch_val = yyjson_obj_get(root, "channel");
    if (ch_val) channel = yyjson_get_str(ch_val);

    yyjson_val *data_val = yyjson_obj_get(root, "data");

    if (!channel || !data_val) {
        yyjson_doc_free(doc);
        return;
    }

    /* Validate channel against whitelist of known channels */
    static const char *KNOWN_CHANNELS[] = {
        "allMids", "l2Book", "candle", "orderUpdates", "userFills", NULL
    };
    bool channel_known = false;
    for (int i = 0; KNOWN_CHANNELS[i]; i++) {
        if (strcmp(channel, KNOWN_CHANNELS[i]) == 0) {
            channel_known = true;
            break;
        }
    }
    if (!channel_known) {
        tb_log_warn("ws: ignoring unknown channel '%s'", channel);
        yyjson_doc_free(doc);
        return;
    }

    if (strcmp(channel, "allMids") == 0 && ws->cbs.on_mids) {
        tb_mid_t mids[TB_MAX_ASSETS];
        int n = 0;
        yyjson_val *mids_obj = yyjson_obj_get(data_val, "mids");
        if (mids_obj) {
            hl_json_parse_all_mids(mids_obj, mids, &n);
        }
        if (n > 0) ws->cbs.on_mids(mids, n, ws->cbs.userdata);
    }
    else if (strcmp(channel, "l2Book") == 0 && ws->cbs.on_book) {
        tb_book_t book;
        memset(&book, 0, sizeof(book));
        if (hl_json_parse_l2_book(data_val, &book) == 0) {
            ws->cbs.on_book(&book, ws->cbs.userdata);
        }
    }
    else if (strcmp(channel, "candle") == 0 && ws->cbs.on_candle) {
        /* Candle data: { "t": ..., "T": ..., ... , "s": "ETH", "i": "15m" } */
        const char *sym = NULL;
        yyjson_val *s_val = yyjson_obj_get(data_val, "s");
        if (s_val) sym = yyjson_get_str(s_val);

        tb_candle_t candle;
        memset(&candle, 0, sizeof(candle));
        /* Simple single candle parse */
        yyjson_val *t_val = yyjson_obj_get(data_val, "t");
        if (t_val) candle.time_open = yyjson_get_sint(t_val);
        yyjson_val *T_val = yyjson_obj_get(data_val, "T");
        if (T_val) candle.time_close = yyjson_get_sint(T_val);

        const char *o = NULL, *h = NULL, *l = NULL, *c = NULL, *v = NULL;
        yyjson_val *tmp;
        tmp = yyjson_obj_get(data_val, "o"); if (tmp) o = yyjson_get_str(tmp);
        tmp = yyjson_obj_get(data_val, "h"); if (tmp) h = yyjson_get_str(tmp);
        tmp = yyjson_obj_get(data_val, "l"); if (tmp) l = yyjson_get_str(tmp);
        tmp = yyjson_obj_get(data_val, "c"); if (tmp) c = yyjson_get_str(tmp);
        tmp = yyjson_obj_get(data_val, "v"); if (tmp) v = yyjson_get_str(tmp);

        if (o) candle.open   = tb_decimal_from_str(o);
        if (h) candle.high   = tb_decimal_from_str(h);
        if (l) candle.low    = tb_decimal_from_str(l);
        if (c) candle.close  = tb_decimal_from_str(c);
        if (v) candle.volume = tb_decimal_from_str(v);

        if (sym) ws->cbs.on_candle(sym, &candle, ws->cbs.userdata);
    }
    else if (strcmp(channel, "orderUpdates") == 0 && ws->cbs.on_order_update) {
        if (yyjson_is_arr(data_val)) {
            tb_order_t orders[100];
            int n = 0;
            hl_json_parse_orders(data_val, orders, &n, 100);
            if (n > 0) ws->cbs.on_order_update(orders, n, ws->cbs.userdata);
        }
    }
    else if (strcmp(channel, "userFills") == 0 && ws->cbs.on_fill) {
        /* Hyperliquid wraps fills: {"isSnapshot":bool,"user":"...","fills":[...]} */
        yyjson_val *fills_arr = data_val;
        if (!yyjson_is_arr(fills_arr)) {
            fills_arr = yyjson_obj_get(data_val, "fills");
        }
        if (fills_arr && yyjson_is_arr(fills_arr)) {
            tb_fill_t fills[100];
            int n = 0;
            hl_json_parse_fills(fills_arr, fills, &n, 100);
            if (n > 0) ws->cbs.on_fill(fills, n, ws->cbs.userdata);
        }
    }

    yyjson_doc_free(doc);
}

/* ── LWS callback ──────────────────────────────────────────────────────────── */
static int lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user __attribute__((unused)), void *in, size_t len) {
    hl_ws_t *ws = (hl_ws_t *)lws_context_user(lws_get_context(wsi));
    if (!ws) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        tb_log_info("WebSocket connected to %s", ws->ws_host);
        ws->connected = true;
        ws->reconnect_attempts = 0;
        ws->reconnect_delay_ms = 1000;

        /* Re-subscribe to saved subscriptions */
        pthread_mutex_lock(&ws->subs_lock);
        for (int i = 0; i < ws->n_subs; i++) {
            ws_queue_push(&ws->send_queue, ws->subs[i]);
        }
        pthread_mutex_unlock(&ws->subs_lock);

        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* Accumulate fragments with overflow protection */
        if (len > 16 * 1024 * 1024 || ws->recv_len > 16 * 1024 * 1024) {
            tb_log_error("ws: message too large (%zu + %zu), dropping",
                         ws->recv_len, len);
            ws->recv_len = 0;
            break;
        }
        size_t needed = ws->recv_len + len;
        if (needed >= ws->recv_cap) {
            size_t new_cap = (needed + 1) * 2;
            if (new_cap < needed) { /* integer overflow check */
                tb_log_error("ws: buffer size overflow");
                ws->recv_len = 0;
                break;
            }
            char *new_buf = realloc(ws->recv_buf, new_cap);
            if (!new_buf) {
                ws->recv_len = 0;
                break;
            }
            ws->recv_buf = new_buf;
            ws->recv_cap = new_cap;
        }
        memcpy(ws->recv_buf + ws->recv_len, in, len);
        ws->recv_len += len;

        if (lws_is_final_fragment(wsi)) {
            ws->recv_buf[ws->recv_len] = '\0';
            dispatch_message(ws, ws->recv_buf, ws->recv_len);
            ws->recv_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        char msg[WS_MSG_MAX];
        if (ws_queue_pop(&ws->send_queue, msg, sizeof(msg))) {
            size_t msg_len = strlen(msg);
            if (msg_len >= WS_MSG_MAX) {
                tb_log_error("ws: message too long (%zu), dropping", msg_len);
                break;
            }
            /* LWS requires LWS_PRE bytes before the message */
            unsigned char buf[LWS_PRE + WS_MSG_MAX];
            memcpy(buf + LWS_PRE, msg, msg_len);
            lws_write(wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);

            /* Check for more messages */
            pthread_mutex_lock(&ws->send_queue.lock);
            bool more = ws->send_queue.count > 0;
            pthread_mutex_unlock(&ws->send_queue.lock);
            if (more) lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        tb_log_error("WebSocket connection error: %s",
                     in ? (const char *)in : "unknown");
        ws->connected = false;
        ws->wsi = NULL;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        tb_log_warn("WebSocket closed");
        ws->connected = false;
        ws->wsi = NULL;
        break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        /* Woken by lws_cancel_service — check if there are queued messages */
        if (ws->wsi && ws->connected) {
            pthread_mutex_lock(&ws->send_queue.lock);
            bool has_msgs = ws->send_queue.count > 0;
            pthread_mutex_unlock(&ws->send_queue.lock);
            if (has_msgs) lws_callback_on_writable(ws->wsi);
        }
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "hyperliquid", lws_callback, 0, 65536, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

/* ── Event loop thread ─────────────────────────────────────────────────────── */
static void *ws_thread_func(void *arg) {
    hl_ws_t *ws = (hl_ws_t *)arg;

    while (ws->running) {
        /* Create LWS context if needed */
        if (!ws->lws_ctx) {
            struct lws_context_creation_info info;
            memset(&info, 0, sizeof(info));
            info.port = CONTEXT_PORT_NO_LISTEN;
            info.protocols = protocols;
            info.user = ws;
            info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

            ws->lws_ctx = lws_create_context(&info);
            if (!ws->lws_ctx) {
                tb_log_error("failed to create LWS context");
                usleep(1000000);
                continue;
            }
        }

        /* Connect if not connected */
        if (!ws->wsi && !ws->connected) {
            struct lws_client_connect_info cci;
            memset(&cci, 0, sizeof(cci));
            cci.context = ws->lws_ctx;
            cci.address = ws->ws_host;
            cci.port = ws->ws_port;
            cci.path = ws->ws_path;
            cci.host = ws->ws_host;
            cci.origin = ws->ws_host;
            cci.protocol = protocols[0].name;
            cci.ssl_connection = ws->use_ssl ? LCCSCF_USE_SSL : 0;

            ws->wsi = lws_client_connect_via_info(&cci);
            if (!ws->wsi) {
                tb_log_warn("WebSocket connect failed, retry in %dms",
                            ws->reconnect_delay_ms);
                usleep(ws->reconnect_delay_ms * 1000);
                /* Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 30s */
                ws->reconnect_delay_ms *= 2;
                if (ws->reconnect_delay_ms > 30000)
                    ws->reconnect_delay_ms = 30000;
                ws->reconnect_attempts++;
                continue;
            }
        }

        /* Service the event loop */
        lws_service(ws->lws_ctx, 50); /* 50ms timeout */

        /* Handle disconnection → reconnect */
        if (!ws->connected && ws->wsi == NULL && ws->running) {
            tb_log_info("reconnecting in %dms...", ws->reconnect_delay_ms);
            usleep(ws->reconnect_delay_ms * 1000);
            ws->reconnect_delay_ms *= 2;
            if (ws->reconnect_delay_ms > 30000)
                ws->reconnect_delay_ms = 30000;
            ws->reconnect_attempts++;
        }
    }

    if (ws->lws_ctx) {
        lws_context_destroy(ws->lws_ctx);
        ws->lws_ctx = NULL;
    }

    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

hl_ws_t *hl_ws_create(const char *ws_url, const hl_ws_callbacks_t *cbs) {
    hl_ws_t *ws = calloc(1, sizeof(hl_ws_t));
    if (!ws) return NULL;

    snprintf(ws->ws_url, sizeof(ws->ws_url), "%s", ws_url);
    parse_ws_url(ws);
    memcpy(&ws->cbs, cbs, sizeof(hl_ws_callbacks_t));

    ws_queue_init(&ws->send_queue);
    pthread_mutex_init(&ws->subs_lock, NULL);

    ws->recv_cap = 65536;
    ws->recv_buf = malloc(ws->recv_cap);
    ws->recv_len = 0;

    ws->reconnect_delay_ms = 1000;

    return ws;
}

void hl_ws_destroy(hl_ws_t *ws) {
    if (!ws) return;
    hl_ws_disconnect(ws);
    free(ws->recv_buf);
    pthread_mutex_destroy(&ws->subs_lock);
    pthread_mutex_destroy(&ws->send_queue.lock);
    free(ws);
}

int hl_ws_connect(hl_ws_t *ws) {
    if (ws->running) return 0;
    ws->running = true;

    if (pthread_create(&ws->thread, NULL, ws_thread_func, ws) != 0) {
        ws->running = false;
        return -1;
    }

    tb_log_info("WebSocket thread started for %s", ws->ws_url);
    return 0;
}

void hl_ws_disconnect(hl_ws_t *ws) {
    if (!ws->running) return;
    ws->running = false;
    pthread_join(ws->thread, NULL);
    ws->connected = false;
    ws->wsi = NULL;
    tb_log_info("WebSocket disconnected");
}

/* ── Input validation ──────────────────────────────────────────────────────── */

/* Reject strings containing JSON-special characters to prevent injection */
static bool is_safe_json_str(const char *s) {
    if (!s) return false;
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
            return false;
    }
    return true;
}

/* ── Subscription helpers ──────────────────────────────────────────────────── */

static int send_subscribe(hl_ws_t *ws, const char *sub_json) {
    char msg[WS_MSG_MAX];
    snprintf(msg, sizeof(msg),
             "{\"method\":\"subscribe\",\"subscription\":%s}", sub_json);

    /* Save for reconnect */
    pthread_mutex_lock(&ws->subs_lock);
    if (ws->n_subs < MAX_SUBS) {
        snprintf(ws->subs[ws->n_subs], WS_MSG_MAX, "%s", msg);
        ws->n_subs++;
    }
    pthread_mutex_unlock(&ws->subs_lock);

    if (!ws_queue_push(&ws->send_queue, msg)) {
        tb_log_warn("WS send queue full");
        return -1;
    }

    /* Wake the LWS service thread — lws_cancel_service is thread-safe,
     * unlike lws_callback_on_writable which must run on the LWS thread. */
    if (ws->lws_ctx) {
        lws_cancel_service(ws->lws_ctx);
    }

    return 0;
}

int hl_ws_subscribe_all_mids(hl_ws_t *ws) {
    return send_subscribe(ws, "{\"type\":\"allMids\"}");
}

int hl_ws_subscribe_l2_book(hl_ws_t *ws, const char *coin) {
    if (!is_safe_json_str(coin)) {
        tb_log_error("ws: invalid coin for l2Book subscribe");
        return -1;
    }
    char sub[128];
    snprintf(sub, sizeof(sub), "{\"type\":\"l2Book\",\"coin\":\"%s\"}", coin);
    return send_subscribe(ws, sub);
}

int hl_ws_subscribe_candle(hl_ws_t *ws, const char *coin, const char *interval) {
    if (!is_safe_json_str(coin) || !is_safe_json_str(interval)) {
        tb_log_error("ws: invalid coin/interval for candle subscribe");
        return -1;
    }
    char sub[128];
    snprintf(sub, sizeof(sub),
             "{\"type\":\"candle\",\"coin\":\"%s\",\"interval\":\"%s\"}",
             coin, interval);
    return send_subscribe(ws, sub);
}

int hl_ws_subscribe_order_updates(hl_ws_t *ws, const char *user_addr) {
    if (!is_safe_json_str(user_addr)) {
        tb_log_error("ws: invalid user address for orderUpdates subscribe");
        return -1;
    }
    char sub[128];
    snprintf(sub, sizeof(sub),
             "{\"type\":\"orderUpdates\",\"user\":\"%s\"}", user_addr);
    return send_subscribe(ws, sub);
}

int hl_ws_subscribe_user_fills(hl_ws_t *ws, const char *user_addr) {
    if (!is_safe_json_str(user_addr)) {
        tb_log_error("ws: invalid user address for userFills subscribe");
        return -1;
    }
    char sub[128];
    snprintf(sub, sizeof(sub),
             "{\"type\":\"userFills\",\"user\":\"%s\"}", user_addr);
    return send_subscribe(ws, sub);
}

bool hl_ws_is_connected(const hl_ws_t *ws) {
    return ws && ws->connected;
}

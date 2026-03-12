#include "exchange/paper_exchange.h"
#include "core/logging.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

/* ── Paper order ───────────────────────────────────────────────────────── */
typedef struct {
    uint64_t         oid;
    uint32_t         asset;
    char             coin[16];
    tb_side_t        side;
    double           limit_px;
    double           size;
    double           orig_size;
    bool             reduce_only;
    tb_tif_t         tif;
    tb_order_type_t  type;
    double           trigger_px;
    bool             active;
    int64_t          timestamp_ms;
    char             cloid[34];
} paper_order_t;

/* ── Paper position ────────────────────────────────────────────────────── */
typedef struct {
    char     coin[16];
    uint32_t asset;
    double   size;        /* negative = short */
    double   entry_px;
    double   unrealized_pnl;
    double   realized_pnl;
    int      leverage;
} paper_position_t;

#define PAPER_MAX_ORDERS    256
#define PAPER_MAX_POSITIONS 64

struct tb_paper_exchange {
    tb_paper_config_t  cfg;
    pthread_mutex_t    lock;

    /* Orders */
    paper_order_t      orders[PAPER_MAX_ORDERS];
    int                n_orders;
    uint64_t           next_oid;

    /* Positions */
    paper_position_t   positions[PAPER_MAX_POSITIONS];
    int                n_positions;

    /* Account */
    double             balance;
    double             daily_pnl;
    double             daily_fees;
    double             total_unrealized;

    /* Fills callback */
    tb_paper_fill_cb   fill_cb;
    void              *fill_cb_data;

    /* Fill counter for tid */
    uint64_t           next_tid;
};

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static paper_position_t *find_position(tb_paper_exchange_t *pe, const char *coin) {
    for (int i = 0; i < pe->n_positions; i++) {
        if (strcmp(pe->positions[i].coin, coin) == 0)
            return &pe->positions[i];
    }
    return NULL;
}

static paper_position_t *get_or_create_position(tb_paper_exchange_t *pe,
                                                  const char *coin, uint32_t asset) {
    paper_position_t *p = find_position(pe, coin);
    if (p) return p;
    if (pe->n_positions >= PAPER_MAX_POSITIONS) return NULL;
    p = &pe->positions[pe->n_positions++];
    memset(p, 0, sizeof(*p));
    strncpy(p->coin, coin, sizeof(p->coin) - 1);
    p->asset = asset;
    return p;
}


/* ── Public API ────────────────────────────────────────────────────────── */

tb_paper_exchange_t *tb_paper_create(const tb_paper_config_t *cfg) {
    tb_paper_exchange_t *pe = calloc(1, sizeof(tb_paper_exchange_t));
    if (!pe) return NULL;

    pe->cfg = *cfg;
    if (pe->cfg.maker_fee_rate == 0) pe->cfg.maker_fee_rate = 0.0002;
    if (pe->cfg.taker_fee_rate == 0) pe->cfg.taker_fee_rate = 0.0005;
    pe->balance = cfg->initial_balance;
    pe->next_oid = 1;
    pe->next_tid = 1;
    pthread_mutex_init(&pe->lock, NULL);

    tb_log_info("paper: created with balance=%.2f", pe->balance);
    return pe;
}

void tb_paper_destroy(tb_paper_exchange_t *pe) {
    if (!pe) return;
    pthread_mutex_destroy(&pe->lock);
    free(pe);
}

void tb_paper_set_fill_cb(tb_paper_exchange_t *pe,
                          tb_paper_fill_cb cb, void *user_data) {
    pe->fill_cb = cb;
    pe->fill_cb_data = user_data;
}

/* Queued fill for deferred callback dispatch outside pe->lock */
#define PAPER_MAX_PENDING_FILLS 32
typedef struct {
    tb_fill_t fills[PAPER_MAX_PENDING_FILLS];
    int       count;
} pending_fills_t;

/* Match orders, collecting fills into a buffer instead of calling callback inline */
static void match_orders_deferred(tb_paper_exchange_t *pe, const char *coin,
                                   double mid, pending_fills_t *pf) {
    for (int i = 0; i < pe->n_orders; i++) {
        paper_order_t *o = &pe->orders[i];
        if (!o->active) continue;
        if (strcmp(o->coin, coin) != 0) continue;

        bool filled = false;
        double fill_px = o->limit_px;
        bool is_taker = false;

        if (o->type == TB_ORDER_TRIGGER) {
            if (o->side == TB_SIDE_BUY && mid >= o->trigger_px) {
                filled = true; fill_px = mid; is_taker = true;
            }
            if (o->side == TB_SIDE_SELL && mid <= o->trigger_px) {
                filled = true; fill_px = mid; is_taker = true;
            }
        } else {
            if (o->side == TB_SIDE_BUY && mid <= o->limit_px) {
                filled = true; fill_px = mid;
            }
            if (o->side == TB_SIDE_SELL && mid >= o->limit_px) {
                filled = true; fill_px = mid;
            }
            if (o->tif == TB_TIF_IOC) {
                if (!filled) { o->active = false; continue; }
                is_taker = true;
            }
        }

        if (filled) {
            if (o->reduce_only) {
                paper_position_t *pos = find_position(pe, coin);
                if (!pos || fabs(pos->size) < 1e-12) { o->active = false; continue; }
                if (o->side == TB_SIDE_BUY && pos->size > 0) { o->active = false; continue; }
                if (o->side == TB_SIDE_SELL && pos->size < 0) { o->active = false; continue; }
            }

            /* Execute fill (updates position/balance internally) */
            double fee_rate = is_taker ? pe->cfg.taker_fee_rate : pe->cfg.maker_fee_rate;
            double notional = fill_px * o->size;
            double fee = notional * fee_rate;
            double pnl = 0;

            paper_position_t *pos = get_or_create_position(pe, o->coin, o->asset);
            if (!pos) { o->active = false; continue; }

            double signed_size = (o->side == TB_SIDE_BUY) ? o->size : -o->size;

            if (fabs(pos->size) < 1e-12) {
                pos->size = signed_size;
                pos->entry_px = fill_px;
            } else if ((pos->size > 0 && o->side == TB_SIDE_SELL) ||
                       (pos->size < 0 && o->side == TB_SIDE_BUY)) {
                double close_size = fmin(fabs(signed_size), fabs(pos->size));
                if (pos->size > 0) pnl = (fill_px - pos->entry_px) * close_size;
                else pnl = (pos->entry_px - fill_px) * close_size;

                double remaining = fabs(pos->size) - close_size;
                if (remaining < 1e-12) {
                    double excess = fabs(signed_size) - close_size;
                    if (excess > 1e-12) {
                        pos->size = (o->side == TB_SIDE_BUY) ? excess : -excess;
                        pos->entry_px = fill_px;
                    } else {
                        pos->size = 0; pos->entry_px = 0;
                    }
                } else {
                    pos->size = (pos->size > 0) ? remaining : -remaining;
                }
                pos->realized_pnl += pnl;
            } else {
                double old_notional = fabs(pos->size) * pos->entry_px;
                double new_notional = o->size * fill_px;
                double total = fabs(pos->size) + o->size;
                pos->entry_px = (total > 1e-9) ?
                    (old_notional + new_notional) / total : fill_px;
                pos->size += signed_size;
            }

            pe->balance += pnl - fee;
            pe->daily_pnl += pnl;
            pe->daily_fees += fee;

            /* Queue the fill for callback after unlock */
            if (pf->count < PAPER_MAX_PENDING_FILLS) {
                tb_fill_t *f = &pf->fills[pf->count++];
                memset(f, 0, sizeof(*f));
                snprintf(f->coin, sizeof(f->coin), "%s", o->coin);
                f->px = tb_decimal_from_double(fill_px, 6);
                f->sz = tb_decimal_from_double(o->size, 6);
                f->side = o->side;
                f->time_ms = now_ms();
                f->closed_pnl = tb_decimal_from_double(pnl, 6);
                f->fee = tb_decimal_from_double(fee, 8);
                f->oid = o->oid;
                f->tid = pe->next_tid++;
                f->crossed = is_taker;
            }

            tb_log_info("paper: fill %s %s %.4f @ %.2f pnl=%.4f fee=%.6f",
                        o->side == TB_SIDE_BUY ? "BUY" : "SELL",
                        o->coin, o->size, fill_px, pnl, fee);

            o->active = false;
        }
    }
}

void tb_paper_feed_mid(tb_paper_exchange_t *pe,
                       const char *coin, double mid_price) {
    pending_fills_t pf = { .count = 0 };

    pthread_mutex_lock(&pe->lock);

    /* Update unrealized P&L for positions in this coin */
    paper_position_t *pos = find_position(pe, coin);
    if (pos && fabs(pos->size) > 1e-12) {
        if (pos->size > 0)
            pos->unrealized_pnl = (mid_price - pos->entry_px) * pos->size;
        else
            pos->unrealized_pnl = (pos->entry_px - mid_price) * fabs(pos->size);
    }

    /* Match orders — fills queued, not dispatched under lock */
    match_orders_deferred(pe, coin, mid_price, &pf);

    /* Recompute total unrealized */
    pe->total_unrealized = 0;
    for (int i = 0; i < pe->n_positions; i++)
        pe->total_unrealized += pe->positions[i].unrealized_pnl;

    pthread_mutex_unlock(&pe->lock);

    /* Fire callbacks AFTER releasing pe->lock to prevent ABBA deadlock
     * (pe->lock → lua_lock vs lua_lock → pe->lock) */
    for (int i = 0; i < pf.count; i++) {
        if (pe->fill_cb)
            pe->fill_cb(&pf.fills[i], pe->fill_cb_data);
    }
}

int tb_paper_place_order(tb_paper_exchange_t *pe,
                         const tb_order_request_t *order,
                         uint64_t *out_oid) {
    pthread_mutex_lock(&pe->lock);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < PAPER_MAX_ORDERS; i++) {
        if (!pe->orders[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&pe->lock);
        return -1;
    }

    paper_order_t *o = &pe->orders[slot];
    memset(o, 0, sizeof(*o));
    o->oid = pe->next_oid++;
    o->asset = order->asset;
    o->side = order->side;
    o->limit_px = tb_decimal_to_double(order->price);
    o->size = tb_decimal_to_double(order->size);
    o->orig_size = o->size;
    o->reduce_only = order->reduce_only;
    o->tif = order->tif;
    o->type = order->type;
    o->trigger_px = tb_decimal_to_double(order->trigger_px);
    o->active = true;
    o->timestamp_ms = now_ms();
    strncpy(o->cloid, order->cloid, sizeof(o->cloid) - 1);

    /* Use coin name from the order request */
    snprintf(o->coin, sizeof(o->coin), "%s", order->coin);

    if (slot >= pe->n_orders) pe->n_orders = slot + 1;
    if (out_oid) *out_oid = o->oid;

    tb_log_info("paper: order %llu %s %.4f @ %.2f (asset %u)",
                (unsigned long long)o->oid,
                o->side == TB_SIDE_BUY ? "BUY" : "SELL",
                o->size, o->limit_px, o->asset);

    pthread_mutex_unlock(&pe->lock);
    return 0;
}

int tb_paper_cancel_order(tb_paper_exchange_t *pe,
                          uint32_t asset, uint64_t oid) {
    (void)asset;
    pthread_mutex_lock(&pe->lock);
    for (int i = 0; i < pe->n_orders; i++) {
        if (pe->orders[i].active && pe->orders[i].oid == oid) {
            pe->orders[i].active = false;
            pthread_mutex_unlock(&pe->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&pe->lock);
    return -1;
}

int tb_paper_get_open_orders(tb_paper_exchange_t *pe,
                             tb_order_t *out_orders, int *out_count,
                             int max_count) {
    pthread_mutex_lock(&pe->lock);
    int n = 0;
    for (int i = 0; i < pe->n_orders; i++) {
        paper_order_t *o = &pe->orders[i];
        if (!o->active) continue;
        if (n >= max_count) break;

        tb_order_t *dst = &out_orders[n++];
        dst->oid = o->oid;
        dst->asset = o->asset;
        strncpy(dst->coin, o->coin, sizeof(dst->coin) - 1);
        dst->side = o->side;
        dst->limit_px = tb_decimal_from_double(o->limit_px, 6);
        dst->sz = tb_decimal_from_double(o->size, 6);
        dst->orig_sz = tb_decimal_from_double(o->orig_size, 6);
        dst->timestamp_ms = o->timestamp_ms;
        dst->reduce_only = o->reduce_only;
        dst->tif = o->tif;
        strncpy(dst->cloid, o->cloid, sizeof(dst->cloid) - 1);
    }
    *out_count = n;
    pthread_mutex_unlock(&pe->lock);
    return 0;
}

int tb_paper_get_positions(tb_paper_exchange_t *pe,
                           tb_position_t *out_positions, int *out_count,
                           int max_count) {
    pthread_mutex_lock(&pe->lock);
    int n = 0;
    for (int i = 0; i < pe->n_positions; i++) {
        paper_position_t *p = &pe->positions[i];
        if (fabs(p->size) < 1e-12) continue;
        if (n >= max_count) break;

        tb_position_t *dst = &out_positions[n++];
        memset(dst, 0, sizeof(*dst));
        strncpy(dst->coin, p->coin, sizeof(dst->coin) - 1);
        dst->asset = p->asset;
        dst->size = tb_decimal_from_double(p->size, 6);
        dst->entry_px = tb_decimal_from_double(p->entry_px, 6);
        dst->unrealized_pnl = tb_decimal_from_double(p->unrealized_pnl, 6);
        dst->realized_pnl = tb_decimal_from_double(p->realized_pnl, 6);
        dst->leverage = p->leverage > 0 ? p->leverage : 1;
    }
    *out_count = n;
    pthread_mutex_unlock(&pe->lock);
    return 0;
}

double tb_paper_get_account_value(tb_paper_exchange_t *pe) {
    pthread_mutex_lock(&pe->lock);
    double val = pe->balance + pe->total_unrealized;
    pthread_mutex_unlock(&pe->lock);
    return val;
}

double tb_paper_get_daily_pnl(tb_paper_exchange_t *pe) {
    pthread_mutex_lock(&pe->lock);
    double pnl = pe->daily_pnl - pe->daily_fees;
    pthread_mutex_unlock(&pe->lock);
    return pnl;
}

void tb_paper_reset_daily(tb_paper_exchange_t *pe) {
    pthread_mutex_lock(&pe->lock);
    pe->daily_pnl = 0;
    pe->daily_fees = 0;
    pthread_mutex_unlock(&pe->lock);
}

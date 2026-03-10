#include "core/types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper: compute 10^n */
static int64_t pow10(uint8_t n) {
    int64_t result = 1;
    for (uint8_t i = 0; i < n; i++) result *= 10;
    return result;
}

int tb_decimal_to_str(tb_decimal_t d, char *buf, size_t buf_len) {
    if (d.scale == 0) {
        return snprintf(buf, buf_len, "%lld", (long long)d.mantissa);
    }

    int64_t divisor = pow10(d.scale);
    int64_t whole = d.mantissa / divisor;
    int64_t frac = d.mantissa % divisor;

    if (d.mantissa < 0 && whole == 0) {
        /* Handle -0.xxx case */
        frac = -frac;
        return snprintf(buf, buf_len, "-0.%0*lld",
                        (int)d.scale, (long long)frac);
    }

    if (frac < 0) frac = -frac;

    /* Format with exact scale digits, then strip trailing zeros */
    int len = snprintf(buf, buf_len, "%lld.%0*lld",
                       (long long)whole, (int)d.scale, (long long)frac);

    /* Strip trailing zeros after decimal point */
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + len - 1;
        while (end > dot && *end == '0') end--;
        if (end == dot) end--; /* Remove dot if no decimals left */
        *(end + 1) = '\0';
        len = (int)(end + 1 - buf);
    }

    return len;
}

tb_decimal_t tb_decimal_from_str(const char *str) {
    tb_decimal_t d = {0, 0};
    if (!str || !str[0]) return d;

    const char *dot = strchr(str, '.');
    if (!dot) {
        d.mantissa = strtoll(str, NULL, 10);
        d.scale = 0;
        return d;
    }

    /* Count decimal places */
    size_t dec_len = strlen(dot + 1);
    if (dec_len > 18) dec_len = 18; /* cap at int64 precision */
    d.scale = (uint8_t)dec_len;

    /* Parse as whole number: remove the dot */
    char tmp[64];
    size_t whole_len = (size_t)(dot - str);
    if (whole_len + dec_len + 1 >= sizeof(tmp)) {
        return d; /* overflow protection */
    }

    memcpy(tmp, str, whole_len);
    memcpy(tmp + whole_len, dot + 1, dec_len);
    tmp[whole_len + dec_len] = '\0';

    d.mantissa = strtoll(tmp, NULL, 10);

    /* Handle negative: "-0.5" → whole_len includes '-', but strtoll handles it */
    if (str[0] == '-' && d.mantissa > 0) {
        d.mantissa = -d.mantissa;
    }

    return d;
}

/* Normalize scales before arithmetic (overflow-safe) */
static void normalize_scales(tb_decimal_t *a, tb_decimal_t *b) {
    if (a->scale == b->scale) return;
    if (a->scale < b->scale) {
        uint8_t diff = b->scale - a->scale;
        if (diff > 18) diff = 18;
        int64_t factor = pow10(diff);
        int64_t result;
        if (__builtin_mul_overflow(a->mantissa, factor, &result)) {
            /* Overflow: reduce target scale instead */
            while (diff > 0 && b->mantissa % 10 == 0) {
                b->mantissa /= 10; b->scale--; diff--;
            }
            a->mantissa *= pow10(diff);
        } else {
            a->mantissa = result;
        }
        a->scale = b->scale;
    } else {
        uint8_t diff = a->scale - b->scale;
        if (diff > 18) diff = 18;
        int64_t factor = pow10(diff);
        int64_t result;
        if (__builtin_mul_overflow(b->mantissa, factor, &result)) {
            while (diff > 0 && a->mantissa % 10 == 0) {
                a->mantissa /= 10; a->scale--; diff--;
            }
            b->mantissa *= pow10(diff);
        } else {
            b->mantissa = result;
        }
        b->scale = a->scale;
    }
}

tb_decimal_t tb_decimal_add(tb_decimal_t a, tb_decimal_t b) {
    normalize_scales(&a, &b);
    tb_decimal_t r = { a.mantissa + b.mantissa, a.scale };
    return r;
}

tb_decimal_t tb_decimal_sub(tb_decimal_t a, tb_decimal_t b) {
    normalize_scales(&a, &b);
    tb_decimal_t r = { a.mantissa - b.mantissa, a.scale };
    return r;
}

tb_decimal_t tb_decimal_mul(tb_decimal_t a, tb_decimal_t b) {
    tb_decimal_t r;
    uint8_t new_scale = a.scale + b.scale;
    if (new_scale > 18) new_scale = 18; /* cap to prevent overflow */
    r.scale = new_scale;
    /* Overflow-safe multiplication */
    if (a.mantissa != 0 && b.mantissa != 0) {
        if (__builtin_mul_overflow(a.mantissa, b.mantissa, &r.mantissa)) {
            /* Overflow: reduce precision on both operands alternately */
            int retries = 0;
            while (retries < 36) {
                if (a.scale > 0 && r.scale > 0) {
                    a.mantissa /= 10; a.scale--; r.scale--;
                } else if (b.scale > 0 && r.scale > 0) {
                    b.mantissa /= 10; b.scale--; r.scale--;
                } else {
                    /* Both at scale 0, reduce mantissa magnitude */
                    if (llabs(a.mantissa) > llabs(b.mantissa))
                        a.mantissa /= 10;
                    else
                        b.mantissa /= 10;
                }
                if (!__builtin_mul_overflow(a.mantissa, b.mantissa, &r.mantissa))
                    break;
                retries++;
            }
        }
    } else {
        r.mantissa = 0;
    }
    /* Reduce scale if too large */
    while (r.scale > 8 && r.mantissa % 10 == 0) {
        r.mantissa /= 10;
        r.scale--;
    }
    return r;
}

tb_decimal_t tb_decimal_div(tb_decimal_t a, tb_decimal_t b) {
    if (b.mantissa == 0) {
        tb_decimal_t r = {0, 0};
        return r;
    }
    /* Scale up numerator for precision, guard against underflow */
    uint8_t extra = 8;
    int16_t result_scale = (int16_t)a.scale + (int16_t)extra - (int16_t)b.scale;
    if (result_scale < 0) {
        /* Reduce extra to avoid negative scale */
        extra = (b.scale > a.scale) ? 0 : (uint8_t)(a.scale - b.scale);
        result_scale = (int16_t)a.scale + (int16_t)extra - (int16_t)b.scale;
        if (result_scale < 0) result_scale = 0;
    }
    if (result_scale > 18) result_scale = 18;

    tb_decimal_t r;
    r.scale = (uint8_t)result_scale;

    /* Overflow-safe scale-up */
    int64_t scaled_a;
    if (__builtin_mul_overflow(a.mantissa, pow10(extra), &scaled_a)) {
        /* Reduce precision to avoid overflow */
        while (extra > 0) {
            extra--;
            result_scale = (int16_t)a.scale + (int16_t)extra - (int16_t)b.scale;
            if (result_scale < 0) result_scale = 0;
            if (!__builtin_mul_overflow(a.mantissa, pow10(extra), &scaled_a))
                break;
        }
        r.scale = (uint8_t)result_scale;
    }

    r.mantissa = scaled_a / b.mantissa;
    return r;
}

int tb_decimal_cmp(tb_decimal_t a, tb_decimal_t b) {
    normalize_scales(&a, &b);
    if (a.mantissa < b.mantissa) return -1;
    if (a.mantissa > b.mantissa) return 1;
    return 0;
}

bool tb_decimal_is_zero(tb_decimal_t d) {
    return d.mantissa == 0;
}

tb_decimal_t tb_decimal_abs(tb_decimal_t d) {
    if (d.mantissa < 0) d.mantissa = -d.mantissa;
    return d;
}

tb_decimal_t tb_decimal_neg(tb_decimal_t d) {
    d.mantissa = -d.mantissa;
    return d;
}

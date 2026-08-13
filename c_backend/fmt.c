#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

long long nova_truncate_i64(double v) {
    const double LIM = 9007199254740991.0; /* 2^53 - 1, mirrors JS engine */
    if (!isfinite(v)) return 0;
    if (v >= LIM) return 9007199254740991LL;
    if (v <= -LIM) return -9007199254740991LL;
    return (long long)v;
}

int nova_is_integral(double v) {
    return isfinite(v) && v == trunc(v) && fabs(v) < 1e15;
}

static void normalize_exponent(char* buf) {
    char* e = strchr(buf, 'e');
    if (!e) return;
    char* p = e + 1;
    char sign = 0;
    if (*p == '+' || *p == '-') { sign = *p; p++; }
    while (*p == '0' && *(p + 1) != '\0') p++; /* strip leading zeros */
    char* w = e + 1;
    if (sign) *w++ = sign;
    while (*p) *w++ = *p++;
    *w = '\0';
}

void format_value(double v, char* out, size_t out_size) {
    if (nova_is_integral(v)) {
        snprintf(out, out_size, "%lld", nova_truncate_i64(v));
        return;
    }
    if (!isfinite(v)) {
        snprintf(out, out_size, "0");
        return;
    }
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    normalize_exponent(buf);
    snprintf(out, out_size, "%s", buf);
}

void format_float_const(double v, char* out, size_t out_size) {
    if (nova_is_integral(v)) {
        snprintf(out, out_size, "%lld.0", nova_truncate_i64(v));
        return;
    }
    format_value(v, out, out_size);
}

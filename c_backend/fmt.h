#ifndef NOVA_FMT_H
#define NOVA_FMT_H

#include <stddef.h>

/* Formats a double exactly like the browser engine serializes numbers:
 * integral values as plain integers, otherwise shortest round-trip decimal
 * (matching JS Number -> String / JSON.stringify semantics). -0 prints "0".
 * Exponents are normalized to JS form ("e-7" not "e-07"). */
void format_value(double v, char* out, size_t out_size);

/* Float-literal constant place used in TAC ("3.0" style for integral floats). */
void format_float_const(double v, char* out, size_t out_size);

long long nova_truncate_i64(double v);
int nova_is_integral(double v);

#endif

#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }

static int is_bin_op(const char *op) {
    return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
           strcmp(op, "/") == 0 || strcmp(op, "%") == 0 || strcmp(op, "==") == 0 ||
           strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 || strcmp(op, "&") == 0 ||
           strcmp(op, "|") == 0 || strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
           strcmp(op, ">>") == 0;
}

static int parse_tac_number(const char *s, long long *out) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (!isdigit((unsigned char)s[i])) return 0;
    *out = strtoll(s, NULL, 10);
    return 1;
}

static int is_plain_place(const char *s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (int i = 1; s[i]; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

static int compute_bin(const char *op, long long a, long long b, long long *out) {
    if (strcmp(op, "+") == 0) { *out = a + b; return 1; }
    if (strcmp(op, "-") == 0) { *out = a - b; return 1; }
    if (strcmp(op, "*") == 0) { *out = a * b; return 1; }
    if (strcmp(op, "/") == 0) { if (b == 0) return 0; *out = a / b; return 1; }
    if (strcmp(op, "%") == 0) { if (b == 0) return 0; *out = a % b; return 1; }
    if (strcmp(op, "==") == 0) { *out = a == b; return 1; }
    if (strcmp(op, "!=") == 0) { *out = a != b; return 1; }
    if (strcmp(op, "<") == 0) { *out = a < b; return 1; }
    if (strcmp(op, ">") == 0) { *out = a > b; return 1; }
    if (strcmp(op, "<=") == 0) { *out = a <= b; return 1; }
    if (strcmp(op, ">=") == 0) { *out = a >= b; return 1; }
    if (strcmp(op, "&") == 0) { *out = (int)a & (int)b; return 1; }
    if (strcmp(op, "|") == 0) { *out = (int)a | (int)b; return 1; }
    if (strcmp(op, "^") == 0) { *out = (int)a ^ (int)b; return 1; }
    if (strcmp(op, "<<") == 0) { *out = (int)a << ((int)b & 31); return 1; }
    if (strcmp(op, ">>") == 0) { *out = (int)a >> ((int)b & 31); return 1; }
    return 0;
}

static int is_pure_op(const char *op) {
    return strcmp(op, "=") == 0 || is_bin_op(op) || strcmp(op, "neg") == 0 ||
           strcmp(op, "!") == 0 || strcmp(op, "~") == 0 || strcmp(op, "CAST_I") == 0 ||
           strcmp(op, "CAST_F") == 0;
}

/* Deep-copy a TAC list */
static void tac_copy(const TacResult *in, TacResult *out) {
    out->count = 0;
    out->capacity = in->count > 0 ? in->count : 1;
    out->items = (TacInstr *)xmalloc(sizeof(TacInstr) * (size_t)out->capacity);
    memcpy(out->items, in->items, sizeof(TacInstr) * (size_t)in->count);
    out->count = in->count;
}

TacResult *nova_optimize(TacResult *in, OptMetrics *metrics) {
    memset(metrics, 0, sizeof(OptMetrics));
    TacResult *list = (TacResult *)xcalloc(1, sizeof(TacResult));
    tac_copy(in, list);

    /* Pass 1: constant folding */
    for (int i = 0; i < list->count; i++) {
        TacInstr *ins = &list->items[i];
        if (is_bin_op(ins->op)) {
            long long a, b;
            if (parse_tac_number(ins->a1, &a) && parse_tac_number(ins->a2, &b)) {
                long long r;
                if (compute_bin(ins->op, a, b, &r)) {
                    strcpy(ins->op, "=");
                    snprintf(ins->a1, sizeof(ins->a1), "%lld", r);
                    ins->a2[0] = '\0';
                    metrics->constant_fold++;
                }
            }
        } else if (strcmp(ins->op, "neg") == 0) {
            long long a;
            if (parse_tac_number(ins->a1, &a)) {
                strcpy(ins->op, "=");
                snprintf(ins->a1, sizeof(ins->a1), "%lld", -a);
                metrics->constant_fold++;
            }
        } else if (strcmp(ins->op, "~") == 0) {
            long long a;
            if (parse_tac_number(ins->a1, &a)) {
                strcpy(ins->op, "=");
                snprintf(ins->a1, sizeof(ins->a1), "%lld", (long long)(~(int)a));
                metrics->constant_fold++;
            }
        }
    }

    /* Pass 2: constant propagation of temps assigned exactly once from a const.
     * assignCount counts ALL assignments to a temp (any op); only temps with a
     * single assignment that is a constant '=' are propagated. This mirrors the
     * JS engine and prevents corrupting temps assigned on multiple control paths. */
    typedef struct { char name[64]; char value[64]; int assign_count; int is_const; } ConstEntry;
    ConstEntry *consts = NULL; int const_count = 0, const_cap = 0;
    for (int i = 0; i < list->count; i++) {
        TacInstr *ins = &list->items[i];
        if (!is_plain_place(ins->res) || ins->res[0] != 't') continue;
        int idx = -1;
        for (int c = 0; c < const_count; c++) if (strcmp(consts[c].name, ins->res) == 0) { idx = c; break; }
        if (idx < 0) {
            if (const_count >= const_cap) {
                const_cap = const_cap ? const_cap * 2 : 32;
                consts = (ConstEntry *)xrealloc(consts, sizeof(ConstEntry) * (size_t)const_cap);
            }
            idx = const_count++;
            memset(&consts[idx], 0, sizeof(ConstEntry));
            strncpy(consts[idx].name, ins->res, 63);
        }
        consts[idx].assign_count++;
        long long v;
        if (strcmp(ins->op, "=") == 0 && parse_tac_number(ins->a1, &v)) {
            strncpy(consts[idx].value, ins->a1, 63);
            consts[idx].is_const = 1;
        } else {
            consts[idx].is_const = 0; /* a non-constant assignment disqualifies it */
        }
    }
    for (int c = 0; c < const_count; c++) {
        if (!(consts[c].is_const && consts[c].assign_count == 1)) consts[c].value[0] = '\0';
    }
    for (int i = 0; i < list->count; i++) {
        TacInstr *ins = &list->items[i];
        if (strcmp(ins->op, "LABEL") == 0 || strcmp(ins->op, "FUNC_BEGIN") == 0 || strcmp(ins->op, "FUNC_END") == 0) continue;
        char *fields[2] = { ins->a1, ins->a2 };
        for (int f = 0; f < 2; f++) {
            for (int c = 0; c < const_count; c++) {
                if (consts[c].value[0] && strcmp(fields[f], consts[c].name) == 0) {
                    strncpy(fields[f], consts[c].value, 63);
                    fields[f][63] = '\0';
                    metrics->constant_prop++;
                    break;
                }
            }
        }
    }
    free(consts);

    /* Pass 3: strength reduction (x*2 -> x+x) */
    for (int i = 0; i < list->count; i++) {
        TacInstr *ins = &list->items[i];
        if (strcmp(ins->op, "*") == 0) {
            if (strcmp(ins->a2, "2") == 0) {
                strcpy(ins->op, "+");
                strncpy(ins->a2, ins->a1, 63);
                metrics->strength_reduce++;
            } else if (strcmp(ins->a1, "2") == 0) {
                char other[64];
                strncpy(other, ins->a2, 63); other[63] = '\0';
                strcpy(ins->op, "+");
                strncpy(ins->a1, other, 63);
                strncpy(ins->a2, other, 63);
                metrics->strength_reduce++;
            }
        }
    }

    /* Pass 4: dead code elimination (fixpoint) */
    for (;;) {
        /* used set */
        char (*used)[64] = NULL; int used_count = 0, used_cap = 0;
        for (int i = 0; i < list->count; i++) {
            const char *ops[2] = { list->items[i].a1, list->items[i].a2 };
            for (int f = 0; f < 2; f++) {
                if (is_plain_place(ops[f]) && ops[f][0] == 't') {
                    int found = 0;
                    for (int u = 0; u < used_count; u++) if (strcmp(used[u], ops[f]) == 0) { found = 1; break; }
                    if (!found) {
                        if (used_count >= used_cap) {
                            used_cap = used_cap ? used_cap * 2 : 64;
                            used = (char (*)[64])xrealloc(used, sizeof(char[64]) * (size_t)used_cap);
                        }
                        strncpy(used[used_count], ops[f], 63);
                        used[used_count][63] = '\0';
                        used_count++;
                    }
                }
            }
        }
        int removedAny = 0;
        TacResult *next = (TacResult *)xcalloc(1, sizeof(TacResult));
        next->capacity = list->count > 0 ? list->count : 1;
        next->items = (TacInstr *)xmalloc(sizeof(TacInstr) * (size_t)next->capacity);
        next->count = 0;
        for (int i = 0; i < list->count; i++) {
            TacInstr *ins = &list->items[i];
            if (is_pure_op(ins->op) && is_plain_place(ins->res) && ins->res[0] == 't') {
                int isUsed = 0;
                for (int u = 0; u < used_count; u++) if (strcmp(used[u], ins->res) == 0) { isUsed = 1; break; }
                if (!isUsed) { metrics->dead_code++; removedAny = 1; continue; }
            }
            next->items[next->count++] = *ins;
        }
        free(used);
        free(list->items);
        free(list);
        list = next;
        if (!removedAny) break;
    }

    int original = in->count;
    metrics->reduction_percentage = original > 0
        ? (double)((long long)((1.0 - (double)list->count / (double)original) * 1000.0 + 0.5)) / 10.0
        : 0.0;

    /* carry over strings & temp types from input (they're shared) */
    list->strings = in->strings;
    list->string_count = in->string_count;
    list->string_capacity = 0; /* not owned */
    list->tempNames = in->tempNames;
    list->tempTypes = in->tempTypes;
    list->tempType_count = in->tempType_count;
    list->tempType_capacity = 0; /* not owned */
    list->tempCount = in->tempCount;
    list->labelCount = in->labelCount;

    return list;
}

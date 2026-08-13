#include "optimizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

static void* xrealloc(void* p, size_t n) { void* q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }

static int is_bin_op(const char* op) {
    return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
           strcmp(op, "/") == 0 || strcmp(op, "%") == 0 || strcmp(op, "==") == 0 ||
           strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0;
}

static int is_pure_op(const char* op) {
    return strcmp(op, "=") == 0 || is_bin_op(op) || strcmp(op, "neg") == 0 || strcmp(op, "!") == 0;
}

/* matches JS parseTacNumber: ^-?\d+$ */
static int parse_tac_number(const char* s, long long* out) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    *out = strtoll(s, NULL, 10);
    return 1;
}

static int is_plain_place(const char* s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (int i = 1; s[i]; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    }
    return 1;
}

static int compute_bin(const char* op, long long a, long long b, long long* out) {
    if (strcmp(op, "+") == 0) { *out = a + b; return 1; }
    if (strcmp(op, "-") == 0) { *out = a - b; return 1; }
    if (strcmp(op, "*") == 0) { *out = a * b; return 1; }
    if (strcmp(op, "/") == 0) {
        if (b == 0) return 0;
        *out = a / b; /* C truncates toward zero, matching the JS engine */
        return 1;
    }
    if (strcmp(op, "%") == 0) {
        if (b == 0) return 0;
        *out = a % b;
        return 1;
    }
    if (strcmp(op, "==") == 0) { *out = a == b ? 1 : 0; return 1; }
    if (strcmp(op, "!=") == 0) { *out = a != b ? 1 : 0; return 1; }
    if (strcmp(op, "<") == 0) { *out = a < b ? 1 : 0; return 1; }
    if (strcmp(op, ">") == 0) { *out = a > b ? 1 : 0; return 1; }
    if (strcmp(op, "<=") == 0) { *out = a <= b ? 1 : 0; return 1; }
    if (strcmp(op, ">=") == 0) { *out = a >= b ? 1 : 0; return 1; }
    return 0;
}

typedef struct { char name[64]; char value[64]; int used; } ConstEntry;

TACList* optimize_tac(const TACList* input, OptimizationMetrics* metrics) {
    memset(metrics, 0, sizeof(OptimizationMetrics));

    TACList* list = tac_list_clone(input);

    /* Pass 1: constant folding */
    for (int i = 0; i < list->count; i++) {
        TACInstr* ins = &list->items[i];
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
        }
    }

    /* Pass 2: constant propagation of single-assignment temps */
    ConstEntry* consts = NULL;
    int const_count = 0, const_cap = 0;
    /* collect candidates */
    for (int i = 0; i < list->count; i++) {
        TACInstr* ins = &list->items[i];
        if (is_plain_place(ins->res) && ins->res[0] == 't') {
            int existing = -1;
            for (int c = 0; c < const_count; c++) {
                if (strcmp(consts[c].name, ins->res) == 0) { existing = c; break; }
            }
            if (strcmp(ins->op, "=") == 0) {
                long long v;
                if (parse_tac_number(ins->a1, &v)) {
                    if (existing < 0) {
                        if (const_count >= const_cap) {
                            const_cap = const_cap ? const_cap * 2 : 16;
                            consts = (ConstEntry*)xrealloc(consts, sizeof(ConstEntry) * (size_t)const_cap);
                        }
                        ConstEntry* e = &consts[const_count++];
                        memset(e, 0, sizeof(ConstEntry));
                        snprintf(e->name, sizeof(e->name), "%s", ins->res);
                        snprintf(e->value, sizeof(e->value), "%s", ins->a1);
                        e->used = 1;
                    }
                    continue;
                }
            }
            /* any other assignment to the same temp disqualifies it if assigned twice */
            if (existing >= 0) consts[existing].used++;
            else {
                if (const_count >= const_cap) {
                    const_cap = const_cap ? const_cap * 2 : 16;
                    consts = (ConstEntry*)xrealloc(consts, sizeof(ConstEntry) * (size_t)const_cap);
                }
                ConstEntry* e = &consts[const_count++];
                memset(e, 0, sizeof(ConstEntry));
                snprintf(e->name, sizeof(e->name), "%s", ins->res);
                e->used = 1; /* track assignment count; value stays empty => not constant */
            }
        }
    }
    /* keep only temps assigned exactly once with a constant value */
    /* (recompute assignment counts precisely) */
    for (int c = 0; c < const_count; c++) {
        int assigns = 0;
        for (int i = 0; i < list->count; i++) {
            if (is_plain_place(list->items[i].res) && strcmp(list->items[i].res, consts[c].name) == 0) assigns++;
        }
        if (assigns != 1 || consts[c].value[0] == '\0') consts[c].value[0] = '\0';
    }
    for (int i = 0; i < list->count; i++) {
        TACInstr* ins = &list->items[i];
        if (strcmp(ins->op, "LABEL") == 0 || strcmp(ins->op, "FUNC_BEGIN") == 0 || strcmp(ins->op, "FUNC_END") == 0) continue;
        char* fields[2] = { ins->a1, ins->a2 };
        for (int f = 0; f < 2; f++) {
            for (int c = 0; c < const_count; c++) {
                if (consts[c].value[0] && strcmp(fields[f], consts[c].name) == 0) {
                    snprintf(fields[f], 64, "%s", consts[c].value);
                    fields[f][63] = '\0';
                    metrics->constant_prop++;
                    break;
                }
            }
        }
    }
    free(consts);

    /* Pass 3: strength reduction (x * 2 -> x + x) */
    for (int i = 0; i < list->count; i++) {
        TACInstr* ins = &list->items[i];
        if (strcmp(ins->op, "*") == 0) {
            if (strcmp(ins->a2, "2") == 0) {
                strcpy(ins->op, "+");
                snprintf(ins->a2, sizeof(ins->a2), "%s", ins->a1);
                metrics->strength_reduce++;
            } else if (strcmp(ins->a1, "2") == 0) {
                strcpy(ins->op, "+");
                char other[64];
                snprintf(other, sizeof(other), "%s", ins->a2);
                other[63] = '\0';
                snprintf(ins->a1, sizeof(ins->a1), "%s", other);
                snprintf(ins->a2, sizeof(ins->a2), "%s", other);
                metrics->strength_reduce++;
            }
        }
    }

    /* Pass 4: dead code elimination (to fixpoint) */
    for (;;) {
        /* mark used temps */
        int removed_any = 0;
        TACList* next = tac_list_clone(list);
        next->count = 0;
        for (int pass = 0; pass < 2; pass++) {
            /* pass 0: build used set; pass 1: copy live instructions */
        }
        char (*used)[64] = NULL;
        int used_count = 0, used_cap = 0;
        for (int i = 0; i < list->count; i++) {
            const char* ops[2] = { list->items[i].a1, list->items[i].a2 };
            for (int f = 0; f < 2; f++) {
                if (is_plain_place(ops[f]) && ops[f][0] == 't') {
                    int found = 0;
                    for (int u = 0; u < used_count; u++) {
                        if (strcmp(used[u], ops[f]) == 0) { found = 1; break; }
                    }
                    if (!found) {
                        if (used_count >= used_cap) {
                            used_cap = used_cap ? used_cap * 2 : 64;
                            used = (char (*)[64])xrealloc(used, sizeof(char[64]) * (size_t)used_cap);
                        }
                        snprintf(used[used_count], 64, "%s", ops[f]);
                        used[used_count][63] = '\0';
                        used_count++;
                    }
                }
            }
        }
        next->count = 0;
        for (int i = 0; i < list->count; i++) {
            TACInstr* ins = &list->items[i];
            if (is_pure_op(ins->op) && is_plain_place(ins->res) && ins->res[0] == 't') {
                int is_used = 0;
                for (int u = 0; u < used_count; u++) {
                    if (strcmp(used[u], ins->res) == 0) { is_used = 1; break; }
                }
                if (!is_used) {
                    metrics->dead_code++;
                    removed_any = 1;
                    continue;
                }
            }
            next->items[next->count++] = *ins;
        }
        free(used);
        tac_list_free(list);
        list = next;
        if (!removed_any) break;
    }

    int original = input->count;
    metrics->reduction_percentage = original > 0
        ? ((double)((long long)((1.0 - (double)list->count / (double)original) * 1000.0 + 0.5))) / 10.0
        : 0.0;

    return list;
}

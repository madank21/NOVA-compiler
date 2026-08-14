#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Shortest round-trip double formatting (matches JS String(v)/JSON.stringify). */
void nova_fmt_shortest(double v, char *out, size_t n) {
    if (isfinite(v) && v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%lld", (long long)v);
        return;
    }
    if (!isfinite(v)) { snprintf(out, n, "0"); return; }
    char tmp[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
        if (strtod(tmp, NULL) == v) break;
    }
    /* normalize exponent like JS (1e-7 not 1e-07) */
    char *e = strchr(tmp, 'e');
    if (e) {
        char *p = e + 1, sign = 0;
        if (*p == '+' || *p == '-') { sign = *p; p++; }
        while (*p == '0' && *(p + 1) != '\0') p++;
        char *w = e + 1;
        if (sign) *w++ = sign;
        while (*p) *w++ = *p++;
        *w = '\0';
    }
    snprintf(out, n, "%s", tmp);
}

void nova_fmt_float_const(double v, char *out, size_t n) {
    if (isfinite(v) && v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%lld.0", (long long)v);
        return;
    }
    nova_fmt_shortest(v, out, n);
}

CompileResult *nova_compile(const char *source, const char **inputs, int input_count) {
    CompileResult *r = (CompileResult *)calloc(1, sizeof(CompileResult));
    if (!r) { fprintf(stderr, "nova: out of memory\n"); exit(1); }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    r->engine = "native-c";
    r->diags = diag_list_new();

    r->tokens = nova_tokenize(source, r->diags);
    r->ast = nova_parse(r->tokens, r->diags);
    r->sem = nova_semantic(r->ast, r->diags);

    if (!diag_has_errors(r->diags)) {
        r->tac = nova_gen_tac(r->ast, r->sem, r->diags);
        r->optTac = nova_optimize(r->tac, &r->metrics);
        r->bytecode = nova_gen_bytecode(r->optTac, r->sem);
        if (!diag_has_errors(r->diags)) {
            r->vm = nova_vm_run(r->bytecode, r->sem, inputs, input_count);
            /* merge runtime diagnostics into the main list */
            for (int i = 0; i < r->vm->runtimeDiags->count; i++) {
                Diag *d = &r->vm->runtimeDiags->items[i];
                diag_add(r->diags, d->level, d->line, d->column, "%s", d->msg);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    r->compile_time_ms = (double)((long long)(ms * 100.0 + 0.5)) / 100.0;
    r->success = !diag_has_errors(r->diags);
    return r;
}

void nova_compile_free(CompileResult *r) {
    if (!r) return;
    if (r->vm) nova_vm_free(r->vm);
    if (r->bytecode) nova_bytecode_free(r->bytecode);
    if (r->optTac) nova_tac_free(r->optTac);
    if (r->tac) nova_tac_free(r->tac);
    if (r->sem) nova_semantic_free(r->sem);
    if (r->ast) nova_ast_free(r->ast);
    if (r->tokens) nova_token_list_free(r->tokens);
    diag_list_free(r->diags);
    free(r);
}

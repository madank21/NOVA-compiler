#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

CompileResult* compile_source(const char* source, const char** inputs, int input_count) {
    CompileResult* r = (CompileResult*)calloc(1, sizeof(CompileResult));
    if (!r) { fprintf(stderr, "nova: out of memory\n"); exit(1); }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    r->engine = "native-c";
    r->pool = strpool_new();
    r->diags = diag_list_new();

    /* Phase 1: lexical analysis */
    r->tokens = tokenize(source, r->diags, r->pool);

    /* Phase 2: parsing */
    r->ast = parse_program(r->tokens, r->diags, r->pool);

    /* Phase 3: semantic analysis + layout */
    r->sem = analyze_semantics(r->ast, r->diags, r->pool);

    if (!diag_has_errors(r->diags)) {
        /* Phase 4: TAC generation */
        r->tac_gen = generate_tac(r->ast, r->sem, r->diags, r->pool);

        /* Phase 5: optimization */
        r->opt_tac = optimize_tac(r->tac_gen->instrs, &r->metrics);

        /* Phase 6: bytecode generation */
        r->bytecode = generate_bytecode(r->opt_tac, r->sem, &r->tac_gen->temp_types,
                                        &r->tac_gen->strings, r->diags);

        /* Phase 7: VM execution */
        if (!diag_has_errors(r->diags)) {
            r->vm = vm_execute(r->bytecode, r->sem, inputs, input_count, r->diags);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double ms = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
    r->compile_time_ms = (double)((long long)(ms * 100.0 + 0.5)) / 100.0;

    r->success = !diag_has_errors(r->diags);
    return r;
}

void compile_result_free(CompileResult* r) {
    if (!r) return;
    if (r->vm) vm_result_free(r->vm);
    if (r->bytecode) bytecode_chunk_free(r->bytecode);
    if (r->opt_tac) tac_list_free(r->opt_tac);
    if (r->tac_gen) tac_gen_free(r->tac_gen);
    if (r->sem) sem_model_free(r->sem);
    if (r->ast) ast_free(r->ast);
    if (r->tokens) token_list_free(r->tokens);
    diag_list_free(r->diags);
    strpool_free(r->pool);
    free(r);
}

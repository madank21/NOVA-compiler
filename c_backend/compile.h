#ifndef NOVA_COMPILE_H
#define NOVA_COMPILE_H

#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "tac.h"
#include "optimizer.h"
#include "bytecode.h"
#include "vm.h"

typedef struct {
    int success;
    const char* engine;         /* "native-c" */
    double compile_time_ms;
    TokenList* tokens;
    ASTNode* ast;
    SemModel* sem;
    TACGen* tac_gen;            /* owns instrs/temp types/strings */
    TACList* opt_tac;
    OptimizationMetrics metrics;
    BytecodeChunk* bytecode;
    VMResult* vm;               /* NULL when compilation failed */
    DiagList* diags;
    StrPool* pool;
} CompileResult;

CompileResult* compile_source(const char* source, const char** inputs, int input_count);
void compile_result_free(CompileResult* r);

/* Serialize the full JSON contract (docs/SCHEMA.md). Caller frees. */
char* serialize_result_json(const CompileResult* r);

#endif
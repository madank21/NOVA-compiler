#ifndef JSON_H
#define JSON_H

#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "tac.h"
#include "optimizer.h"
#include "bytecode.h"
#include "vm.h"

char* serialize_compilation_result(
    TokenList* tokens,
    ASTNode* ast,
    SymbolTable* st,
    TACList* tac,
    TACList* opt_tac,
    OptimizationMetrics* metrics,
    BytecodeChunk* bytecode,
    VMExecutionTrace* trace,
    double compile_time_ms
);

#endif // JSON_H

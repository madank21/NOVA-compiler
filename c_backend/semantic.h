#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "parser.h"

typedef struct Symbol {
    char name[256];
    char type[64];
    int scope_level;
    int address;
    int is_function;
    int param_count;
    struct Symbol* next;
} Symbol;

typedef struct Scope {
    Symbol* symbols;
    int level;
    char name[64];
    struct Scope* parent;
} Scope;

typedef struct {
    Scope* current_scope;
    Scope* global_scope;
    int total_symbols;
    int error_count;
} SymbolTable;

SymbolTable* semantic_analyze(ASTNode* root);
void symbol_table_free(SymbolTable* st);

#endif // SEMANTIC_H

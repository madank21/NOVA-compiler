#include "semantic.h"

static Scope* create_scope(const char* name, int level, Scope* parent) {
    Scope* s = (Scope*)calloc(1, sizeof(Scope));
    strncpy(s->name, name, 63);
    s->name[63] = '\0';
    s->level = level;
    s->parent = parent;
    s->next = NULL;
    return s;
}

static Symbol* lookup_symbol_scope(Scope* scope, const char* name) {
    while (scope) {
        Symbol* sym = scope->symbols;
        while (sym) {
            if (strcmp(sym->name, name) == 0) return sym;
            sym = sym->next;
        }
        scope = scope->parent;
    }
    return NULL;
}

static void add_symbol_scope(Scope* scope, const char* name, const char* type, int is_func, int param_count) {
    Symbol* sym = (Symbol*)calloc(1, sizeof(Symbol));
    strncpy(sym->name, name, 255);
    sym->name[255] = '\0';
    strncpy(sym->type, type, 63);
    sym->type[63] = '\0';
    sym->scope_level = scope->level;
    sym->address = 0;
    sym->is_function = is_func;
    sym->param_count = param_count;
    sym->next = scope->symbols;
    scope->symbols = sym;
}

static void traverse_ast_semantic(ASTNode* node, SymbolTable* st) {
    if (!node) return;

    if (node->type == NODE_FUNCTION_DEF) {
        add_symbol_scope(st->current_scope, node->identifier, node->type_name, 1, node->child_count);
        Scope* func_scope = create_scope(node->identifier, st->current_scope->level + 1, st->current_scope);
        st->current_scope = func_scope;

        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]->type == NODE_PARAMETER) {
                add_symbol_scope(st->current_scope, node->children[i]->identifier, node->children[i]->type_name, 0, 0);
            }
        }

        if (node->right) traverse_ast_semantic(node->right, st);
        st->current_scope = st->current_scope->parent;
        return;
    }

    if (node->type == NODE_VAR_DECL) {
        add_symbol_scope(st->current_scope, node->identifier, node->type_name, 0, 0);
        if (node->left) traverse_ast_semantic(node->left, st);
        return;
    }

    if (node->type == NODE_IDENTIFIER) {
        Symbol* sym = lookup_symbol_scope(st->current_scope, node->identifier);
        if (!sym && strcmp(node->identifier, "printf") != 0 && strcmp(node->identifier, "scanf") != 0) {
            st->error_count++;
        }
    }

    if (node->left) traverse_ast_semantic(node->left, st);
    if (node->right) traverse_ast_semantic(node->right, st);
    if (node->condition) traverse_ast_semantic(node->condition, st);
    if (node->else_branch) traverse_ast_semantic(node->else_branch, st);
    if (node->init) traverse_ast_semantic(node->init, st);
    if (node->increment) traverse_ast_semantic(node->increment, st);

    for (int i = 0; i < node->child_count; i++) {
        traverse_ast_semantic(node->children[i], st);
    }
}

SymbolTable* semantic_analyze(ASTNode* root) {
    SymbolTable* st = (SymbolTable*)calloc(1, sizeof(SymbolTable));
    st->global_scope = create_scope("global", 0, NULL);
    st->scope_list = st->global_scope;
    st->current_scope = st->global_scope;

    // Add built-ins
    add_symbol_scope(st->global_scope, "printf", "int", 1, 1);
    add_symbol_scope(st->global_scope, "scanf", "int", 1, 1);

    traverse_ast_semantic(root, st);
    return st;
}

static void free_scope(Scope* s) {
    if (!s) return;
    Symbol* sym = s->symbols;
    while (sym) {
        Symbol* next = sym->next;
        free(sym);
        sym = next;
    }
    free(s);
}

void symbol_table_free(SymbolTable* st) {
    if (!st) return;
    Scope* scope = st->scope_list;
    while (scope) {
        Scope* next = scope->next;
        free_scope(scope);
        scope = next;
    }
    free(st);
}

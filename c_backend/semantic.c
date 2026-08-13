#include "semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void* xmalloc(size_t n) {
    void* p = malloc(n);
    if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return p;
}
static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n);
    if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return q;
}
static void* xcalloc(size_t n, size_t sz) {
    void* p = calloc(n, sz);
    if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return p;
}

static void reclist_init(RecList* l) {
    l->capacity = 8;
    l->count = 0;
    l->items = (SymRec*)xmalloc(sizeof(SymRec) * (size_t)l->capacity);
}

static SymRec* reclist_add(RecList* l) {
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->items = (SymRec*)xrealloc(l->items, sizeof(SymRec) * (size_t)l->capacity);
    }
    SymRec* r = &l->items[l->count++];
    memset(r, 0, sizeof(SymRec));
    return r;
}

/* Last declaration wins — mirrors the JS engine's Map semantics where a
 * redeclaration in a new block scope overwrites the frame entry. Scope
 * lists hold unique names, so first == last there. */
static SymRec* reclist_find(RecList* l, const char* name) {
    SymRec* found = NULL;
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) found = &l->items[i];
    }
    return found;
}

SymRec* reclist_find_pub(RecList* l, const char* name) {
    return reclist_find(l, name);
}

static void reclist_free(RecList* l) { free(l->items); l->items = NULL; l->count = l->capacity = 0; }

static void push_symbol(SemModel* m, const char* scope, const char* name, const char* kind,
                        const char* type, int offset, int params) {
    if (m->symbol_count >= m->symbol_capacity) {
        m->symbol_capacity = m->symbol_capacity ? m->symbol_capacity * 2 : 32;
        m->symbols = (SymbolRow*)xrealloc(m->symbols, sizeof(SymbolRow) * (size_t)m->symbol_capacity);
    }
    SymbolRow* row = &m->symbols[m->symbol_count++];
    memset(row, 0, sizeof(SymbolRow));
    snprintf(row->scope, sizeof(row->scope), "%s", scope);
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->kind = kind;
    snprintf(row->type, sizeof(row->type), "%s", type);
    snprintf(row->address, sizeof(row->address), "0x%04X", (unsigned)(offset * 4));
    row->params = params;
}

static FuncDef* push_function(SemModel* m) {
    if (m->func_count >= m->func_capacity) {
        m->func_capacity = m->func_capacity ? m->func_capacity * 2 : 8;
        m->functions = (FuncDef*)xrealloc(m->functions, sizeof(FuncDef) * (size_t)m->func_capacity);
    }
    FuncDef* f = &m->functions[m->func_count++];
    memset(f, 0, sizeof(FuncDef));
    reclist_init(&f->frame);
    return f;
}

static StructDef* push_struct(SemModel* m) {
    if (m->struct_count >= m->struct_capacity) {
        m->struct_capacity = m->struct_capacity ? m->struct_capacity * 2 : 8;
        m->structs = (StructDef*)xrealloc(m->structs, sizeof(StructDef) * (size_t)m->struct_capacity);
    }
    StructDef* s = &m->structs[m->struct_count++];
    memset(s, 0, sizeof(StructDef));
    s->field_capacity = 8;
    s->fields = (FieldDef*)xmalloc(sizeof(FieldDef) * (size_t)s->field_capacity);
    return s;
}

RecList* sem_find_function(SemModel* m, const char* name) {
    FuncDef* f = sem_get_function(m, name);
    return f ? &f->frame : NULL;
}

FuncDef* sem_get_function(SemModel* m, const char* name) {
    for (int i = 0; i < m->func_count; i++) {
        if (strcmp(m->functions[i].name, name) == 0) return &m->functions[i];
    }
    return NULL;
}

SymRec* sem_find_global(SemModel* m, const char* name) {
    return reclist_find(&m->globals, name);
}

SymRec* sem_find_in_frame(RecList* frame, const char* name) {
    return frame ? reclist_find(frame, name) : NULL;
}

StructDef* sem_get_struct(SemModel* m, const char* name) {
    for (int i = 0; i < m->struct_count; i++) {
        if (strcmp(m->structs[i].name, name) == 0) return &m->structs[i];
    }
    return NULL;
}

static long long truncate_i64(double v) {
    /* Mirrors the JS engine's truncateToInteger bounds. */
    const double LIM = 9007199254740991.0; /* 2^53 - 1 */
    if (v >= LIM) return 9007199254740991LL;
    if (v <= -LIM) return -9007199254740991LL;
    return (long long)v;
}

static int node_array_size(const ASTNode* node) {
    if (node->has_size) {
        long long v = truncate_i64(node->children[0]->num_val);
        return v > 1 ? (int)v : 1;
    }
    return node->child_count > 1 ? node->child_count : 1;
}

/* ------------------------------------------------------------------------- */
/* Expression typing (mirrors JS exprType; also resolves identifiers)         */
/* ------------------------------------------------------------------------- */

typedef struct ScopeScope {
    RecList recs;
    struct ScopeScope* parent;
} Scope;

typedef struct {
    SemModel* m;
    FuncDef* func;
    Scope* scope_top;
} TypeCtx;

static SymRec* scope_lookup(Scope* top, const char* name) {
    for (Scope* s = top; s; s = s->parent) {
        SymRec* r = reclist_find(&s->recs, name);
        if (r) return r;
    }
    return NULL;
}

/* Sentinel records returned by resolve() for functions/builtins/phantoms. */
static const char* resolve_identifier(TypeCtx* ctx, const char* name, int line, SymRec** out) {
    SemModel* m = ctx->m;
    SymRec* r = scope_lookup(ctx->scope_top, name);
    if (r) { *out = r; return r->type; }
    r = sem_find_global(m, name);
    if (r) { *out = r; return r->type; }
    if (sem_get_function(m, name)) { *out = NULL; return NULL; /* function marker */ }
    if (strcmp(name, "printf") == 0 || strcmp(name, "scanf") == 0) { *out = NULL; return NULL; }
    diag_add(m->diags, "error", line, 1, "Undefined identifier '%s'", name);
    *out = NULL;
    return "int"; /* phantom */
}

static int is_ptr_type(const char* t) {
    size_t n = strlen(t);
    return n > 0 && t[n - 1] == '*';
}

static char* type_dup(TypeCtx* ctx, const char* s) {
    return strpool_dup(ctx->m->pool, s);
}

static char* type_concat(TypeCtx* ctx, const char* a, const char* b) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s%s", a, b);
    return strpool_dup(ctx->m->pool, buf);
}

static char* type_strip_ptr(TypeCtx* ctx, const char* t) {
    size_t n = strlen(t);
    if (n == 0 || t[n - 1] != '*') return type_dup(ctx, t);
    return strpool_ndup(ctx->m->pool, t, (int)n - 1);
}

static const char* expr_type(TypeCtx* ctx, ASTNode* node);

static const char* expr_type(TypeCtx* ctx, ASTNode* node) {
    SemModel* m = ctx->m;
    if (!node) return "int";
    if (strcmp(node->node_type, "NODE_INT_LITERAL") == 0) return "int";
    if (strcmp(node->node_type, "NODE_FLOAT_LITERAL") == 0) return "double";
    if (strcmp(node->node_type, "NODE_STRING_LITERAL") == 0) return "char*";
    if (strcmp(node->node_type, "NODE_IDENTIFIER") == 0) {
        SymRec* rec = NULL;
        const char* t = resolve_identifier(ctx, node->identifier, node->line, &rec);
        return t ? t : "int";
    }
    if (strcmp(node->node_type, "NODE_UNARY_OP") == 0) {
        if (strcmp(node->op, "&") == 0) {
            ASTNode* inner = node->child_count > 0 ? node->children[0] : NULL;
            if (inner && strcmp(inner->node_type, "NODE_IDENTIFIER") == 0) {
                SymRec* rec = NULL;
                const char* t = resolve_identifier(ctx, inner->identifier, inner->line, &rec);
                const char* base = (t && rec) ? rec->type : "int";
                return type_concat(ctx, base, "*");
            }
            return "int*";
        }
        if (strcmp(node->op, "*") == 0) {
            const char* t = expr_type(ctx, node->children[0]);
            return is_ptr_type(t) ? type_strip_ptr(ctx, t) : "int";
        }
        if (strcmp(node->op, "!") == 0) return "int";
        if (strcmp(node->op, "-") == 0 || strcmp(node->op, "++") == 0 || strcmp(node->op, "--") == 0 ||
            strcmp(node->op, "p++") == 0 || strcmp(node->op, "p--") == 0) {
            return expr_type(ctx, node->children[0]);
        }
        return "int";
    }
    if (strcmp(node->node_type, "NODE_BINARY_OP") == 0) {
        const char* op = node->op;
        if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
            strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
            strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) return "int";
        const char* lt = expr_type(ctx, node->children[0]);
        const char* rt = expr_type(ctx, node->children[1]);
        if (strcmp(lt, "float") == 0 || strcmp(lt, "double") == 0 ||
            strcmp(rt, "float") == 0 || strcmp(rt, "double") == 0) return "double";
        return "int";
    }
    if (strcmp(node->node_type, "NODE_INDEX") == 0) {
        const char* base_t = expr_type(ctx, node->children[0]);
        return is_ptr_type(base_t) ? type_strip_ptr(ctx, base_t) : base_t;
    }
    if (strcmp(node->node_type, "NODE_MEMBER") == 0) {
        ASTNode* base = node->child_count > 0 ? node->children[0] : NULL;
        if (base && strcmp(base->node_type, "NODE_IDENTIFIER") == 0) {
            SymRec* rec = NULL;
            const char* t = resolve_identifier(ctx, base->identifier, base->line, &rec);
            const char* tname = (t && rec) ? rec->type : "";
            if (strncmp(tname, "struct ", 7) == 0) {
                StructDef* s = sem_get_struct(m, tname + 7);
                if (s) {
                    for (int i = 0; i < s->field_count; i++) {
                        if (strcmp(s->fields[i].name, node->identifier) == 0) {
                            return s->fields[i].is_array
                                ? type_concat(ctx, s->fields[i].type, "*")
                                : s->fields[i].type;
                        }
                    }
                    diag_add(m->diags, "error", node->line, 1,
                             "Struct '%s' has no field '%s'", tname + 7, node->identifier);
                    return "int";
                }
            }
        }
        diag_add(m->diags, "error", node->line, 1, "Member access on non-struct expression");
        return "int";
    }
    if (strcmp(node->node_type, "NODE_FUNC_CALL") == 0) {
        if (strcmp(node->identifier, "printf") == 0 || strcmp(node->identifier, "scanf") == 0) return "int";
        FuncDef* f = sem_get_function(m, node->identifier);
        if (!f) {
            SymRec* rec = NULL;
            const char* t = resolve_identifier(ctx, node->identifier, node->line, &rec);
            if (t && rec) {
                diag_add(m->diags, "error", node->line, 1, "'%s' is not a defined function", node->identifier);
            }
            return "int";
        }
        if (f->param_count != node->child_count) {
            diag_add(m->diags, "error", node->line, 1,
                     "Function '%s' expects %d argument(s), got %d",
                     node->identifier, f->param_count, node->child_count);
        }
        return f->type_name;
    }
    if (strcmp(node->node_type, "NODE_ASSIGNMENT") == 0 || strcmp(node->node_type, "NODE_COMPOUND_ASSIGN") == 0) {
        return expr_type(ctx, node->children[0]);
    }
    return "int";
}

/* ------------------------------------------------------------------------- */
/* Statement walking (declaration collection + checks)                        */
/* ------------------------------------------------------------------------- */

static void walk_expr(TypeCtx* ctx, ASTNode* node);

static void check_assignable(TypeCtx* ctx, ASTNode* node) {
    if (!node) return;
    if (strcmp(node->node_type, "NODE_IDENTIFIER") == 0 || strcmp(node->node_type, "NODE_INDEX") == 0 ||
        strcmp(node->node_type, "NODE_MEMBER") == 0) return;
    if (strcmp(node->node_type, "NODE_UNARY_OP") == 0 && strcmp(node->op, "*") == 0) return;
    diag_add(ctx->m->diags, "error", node->line, 1, "Assignment to non-lvalue");
}

static void walk_expr(TypeCtx* ctx, ASTNode* node) {
    if (!node) return;
    if (strcmp(node->node_type, "NODE_IDENTIFIER") == 0) {
        SymRec* rec = NULL;
        resolve_identifier(ctx, node->identifier, node->line, &rec);
        return;
    }
    if (strcmp(node->node_type, "NODE_ASSIGNMENT") == 0 || strcmp(node->node_type, "NODE_COMPOUND_ASSIGN") == 0) {
        check_assignable(ctx, node->children[0]);
        walk_expr(ctx, node->children[0]);
        walk_expr(ctx, node->children[1]);
        return;
    }
    if (strcmp(node->node_type, "NODE_FUNC_CALL") == 0) {
        expr_type(ctx, node);
        for (int i = 0; i < node->child_count; i++) walk_expr(ctx, node->children[i]);
        return;
    }
    for (int i = 0; i < node->child_count; i++) walk_expr(ctx, node->children[i]);
}

static void declare_local(TypeCtx* ctx, const char* name, const char* type, ASTNode* node, int is_param);

static int type_size(TypeCtx* ctx, const char* type) {
    if (strncmp(type, "struct ", 7) == 0) {
        StructDef* s = sem_get_struct(ctx->m, type + 7);
        return s ? s->size : 1;
    }
    return 1;
}

static void declare_local(TypeCtx* ctx, const char* name, const char* type, ASTNode* node, int is_param) {
    SemModel* m = ctx->m;
    Scope* scope = ctx->scope_top;
    if (reclist_find(&scope->recs, name)) {
        diag_add(m->diags, "error", node->line, 1, "Duplicate declaration of '%s'", name);
        return;
    }
    int size = node->is_array ? node_array_size(node) : type_size(ctx, type);
    SymRec* rec = reclist_add(&scope->recs);
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    snprintf(rec->type, sizeof(rec->type), "%s", type);
    rec->is_array = node->is_array;
    rec->size = size;
    rec->is_global = 0;
    rec->offset = ctx->func->frame_size;
    rec->is_param = is_param;
    /* frame order == slot order */
    SymRec* f = reclist_add(&ctx->func->frame);
    *f = *rec;
    ctx->func->frame_size += size;
    push_symbol(m, ctx->func->name, name,
                node->is_array ? "Array" : (is_param ? "Parameter" : "Variable"),
                type, rec->offset, 0);
}

static void walk_stmt(TypeCtx* ctx, ASTNode* node, int in_loop);

static void walk_stmt(TypeCtx* ctx, ASTNode* node, int in_loop) {
    SemModel* m = ctx->m;
    if (!node) return;
    const char* nt = node->node_type;

    if (strcmp(nt, "NODE_DECL_LIST") == 0) {
        for (int i = 0; i < node->child_count; i++) walk_stmt(ctx, node->children[i], in_loop);
        return;
    }
    if (strcmp(nt, "NODE_VAR_DECL") == 0) {
        declare_local(ctx, node->identifier, node->type_name, node, 0);
        int start = node->is_array && node->has_size ? 1 : 0;
        for (int i = start; i < node->child_count; i++) walk_expr(ctx, node->children[i]);
        return;
    }
    if (strcmp(nt, "NODE_COMPOUND_STMT") == 0) {
        Scope scope;
        reclist_init(&scope.recs);
        scope.parent = ctx->scope_top;
        ctx->scope_top = &scope;
        for (int i = 0; i < node->child_count; i++) walk_stmt(ctx, node->children[i], in_loop);
        ctx->scope_top = scope.parent;
        reclist_free(&scope.recs);
        return;
    }
    if (strcmp(nt, "NODE_IF_STMT") == 0) {
        walk_expr(ctx, node->children[0]);
        walk_stmt(ctx, node->children[1], in_loop);
        if (node->child_count > 2) walk_stmt(ctx, node->children[2], in_loop);
        return;
    }
    if (strcmp(nt, "NODE_WHILE_STMT") == 0) {
        walk_expr(ctx, node->children[0]);
        walk_stmt(ctx, node->children[1], 1);
        return;
    }
    if (strcmp(nt, "NODE_FOR_STMT") == 0) {
        Scope scope;
        reclist_init(&scope.recs);
        scope.parent = ctx->scope_top;
        ctx->scope_top = &scope;
        walk_stmt(ctx, node->children[0], 1);
        walk_expr(ctx, node->children[1]);
        walk_expr(ctx, node->children[2]);
        walk_stmt(ctx, node->children[3], 1);
        ctx->scope_top = scope.parent;
        reclist_free(&scope.recs);
        return;
    }
    if (strcmp(nt, "NODE_BREAK_STMT") == 0 || strcmp(nt, "NODE_CONTINUE_STMT") == 0) {
        if (!in_loop) {
            diag_add(m->diags, "error", node->line, 1, "'%s' used outside of a loop",
                     strcmp(nt, "NODE_BREAK_STMT") == 0 ? "break" : "continue");
        }
        return;
    }
    if (strcmp(nt, "NODE_RETURN_STMT") == 0) {
        for (int i = 0; i < node->child_count; i++) walk_expr(ctx, node->children[i]);
        return;
    }
    if (strcmp(nt, "NODE_EXPRESSION_STMT") == 0) {
        for (int i = 0; i < node->child_count; i++) walk_expr(ctx, node->children[i]);
        return;
    }
    for (int i = 0; i < node->child_count; i++) walk_stmt(ctx, node->children[i], in_loop);
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

SemModel* analyze_semantics(ASTNode* ast, DiagList* diags, StrPool* pool) {
    SemModel* m = (SemModel*)xcalloc(1, sizeof(SemModel));
    m->diags = diags;
    m->pool = pool;
    reclist_init(&m->globals);

    /* Built-ins first (stable UI order). */
    push_symbol(m, "global", "printf", "Function", "int", 0, -1);
    push_symbol(m, "global", "scanf", "Function", "int", 0, -1);

    /* Pass 1a: structs */
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* node = ast->children[i];
        if (strcmp(node->node_type, "NODE_STRUCT_DEF") != 0) continue;
        if (sem_get_struct(m, node->identifier)) {
            diag_add(diags, "error", node->line, 1, "Redefinition of struct '%s'", node->identifier);
            continue;
        }
        StructDef* s = push_struct(m);
        snprintf(s->name, sizeof(s->name), "%s", node->identifier);
        int offset = 0;
        for (int f = 0; f < node->child_count; f++) {
            ASTNode* field = node->children[f];
            if (s->field_count >= s->field_capacity) {
                s->field_capacity *= 2;
                s->fields = (FieldDef*)xrealloc(s->fields, sizeof(FieldDef) * (size_t)s->field_capacity);
            }
            FieldDef* fd = &s->fields[s->field_count++];
            memset(fd, 0, sizeof(FieldDef));
            snprintf(fd->name, sizeof(fd->name), "%s", field->identifier);
            snprintf(fd->type, sizeof(fd->type), "%s", field->type_name);
            fd->is_array = field->is_array;
            fd->size = field->is_array
                ? (field->has_size ? (int)(truncate_i64(field->children[0]->num_val) > 1
                                         ? truncate_i64(field->children[0]->num_val) : 1)
                   : 1)
                : 1;
            fd->offset = offset;
            offset += fd->size;
        }
        s->size = offset;
        push_symbol(m, "global", node->identifier, "Struct", "struct", 0, s->field_count);
    }

    /* Pass 1b: functions and globals in source order */
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* raw = ast->children[i];
        ASTNode** decl_nodes;
        int decl_count;
        if (strcmp(raw->node_type, "NODE_DECL_LIST") == 0) {
            decl_nodes = raw->children;
            decl_count = raw->child_count;
        } else {
            decl_nodes = &raw;
            decl_count = 1;
        }
        for (int d = 0; d < decl_count; d++) {
            ASTNode* node = decl_nodes[d];
            if (strcmp(node->node_type, "NODE_FUNCTION_DEF") == 0) {
                if (sem_get_function(m, node->identifier)) {
                    diag_add(diags, "error", node->line, 1, "Redefinition of function '%s'", node->identifier);
                    continue;
                }
                FuncDef* f = push_function(m);
                snprintf(f->name, sizeof(f->name), "%s", node->identifier);
                snprintf(f->type_name, sizeof(f->type_name), "%s", node->type_name);
                f->node = node;
                for (int c = 0; c < node->child_count; c++) {
                    if (strcmp(node->children[c]->node_type, "NODE_PARAMETER") == 0) f->param_count++;
                }
                push_symbol(m, "global", node->identifier, "Function", node->type_name, 0, f->param_count);
            } else if (strcmp(node->node_type, "NODE_VAR_DECL") == 0) {
                if (sem_find_global(m, node->identifier)) {
                    diag_add(diags, "error", node->line, 1, "Redefinition of global '%s'", node->identifier);
                    continue;
                }
                int size = node->is_array ? node_array_size(node) : type_size(&(TypeCtx){m, NULL, NULL}, node->type_name);
                SymRec* rec = reclist_add(&m->globals);
                snprintf(rec->name, sizeof(rec->name), "%s", node->identifier);
                snprintf(rec->type, sizeof(rec->type), "%s", node->type_name);
                rec->is_array = node->is_array;
                rec->size = size;
                rec->is_global = 1;
                rec->offset = m->global_slot_count;
                m->global_slot_count += size;
                push_symbol(m, "global", node->identifier, node->is_array ? "Array" : "Variable",
                            node->type_name, rec->offset, 0);
            }
        }
    }

    if (!sem_get_function(m, "main")) {
        diag_add(diags, "error", 1, 1, "No 'main' function defined");
    }

    /* Pass 2: bodies */
    for (int fi = 0; fi < m->func_count; fi++) {
        FuncDef* f = &m->functions[fi];
        TypeCtx ctx;
        ctx.m = m;
        ctx.func = f;
        Scope global_scope;
        reclist_init(&global_scope.recs);
        global_scope.parent = NULL;
        ctx.scope_top = &global_scope;

        /* params */
        int pi = 0;
        for (int c = 0; c < f->node->child_count; c++) {
            ASTNode* pnode = f->node->children[c];
            if (strcmp(pnode->node_type, "NODE_PARAMETER") != 0) continue;
            ASTNode stub;
            memset(&stub, 0, sizeof(ASTNode));
            stub.line = f->node->line;
            declare_local(&ctx, pnode->identifier, pnode->type_name, &stub, 1);
            pi++;
        }

        ASTNode* body = NULL;
        for (int c = 0; c < f->node->child_count; c++) {
            if (strcmp(f->node->children[c]->node_type, "NODE_PARAMETER") != 0) {
                body = f->node->children[c];
                break;
            }
        }
        walk_stmt(&ctx, body, 0);

        reclist_free(&global_scope.recs);
    }

    return m;
}

void sem_model_free(SemModel* m) {
    if (!m) return;
    reclist_free(&m->globals);
    for (int i = 0; i < m->func_count; i++) reclist_free(&m->functions[i].frame);
    free(m->functions);
    for (int i = 0; i < m->struct_count; i++) free(m->structs[i].fields);
    free(m->structs);
    free(m->symbols);
    free(m);
}

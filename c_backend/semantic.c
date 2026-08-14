#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- Built-in functions ---------------------------- */

typedef struct { const char *name; const char *type; int params; } BuiltinFn;

static const BuiltinFn BUILTINS[] = {
    { "printf", "int", -1 }, { "scanf", "int", -1 },
    { "abs", "int", 1 }, { "assert", "int", 1 },
    { "ceil", "double", 1 }, { "cos", "double", 1 },
    { "exit", "int", 1 }, { "exp", "double", 1 },
    { "fabs", "double", 1 }, { "floor", "double", 1 },
    { "fmod", "double", 2 }, { "log", "double", 1 },
    { "pow", "double", 2 }, { "rand", "int", 0 },
    { "sin", "double", 1 }, { "sqrt", "double", 1 },
    { "srand", "int", 1 }, { "tan", "double", 1 },
    { "time", "int", 1 }
};
static const int BUILTIN_COUNT = (int)(sizeof(BUILTINS) / sizeof(BUILTINS[0]));

static const char *UNSUPPORTED_FNS[] = {
    "malloc", "calloc", "realloc", "free",
    "fopen", "fclose", "fprintf", "fscanf", "fgets", "fputs", "fread", "fwrite",
    "fseek", "ftell", "rewind", "fflush", "setbuf", "setvbuf", "tmpfile", "tmpnam",
    "sprintf", "snprintf", "sscanf", "vprintf", "vfprintf",
    "memcpy", "memset", "memmove", "memcmp",
    "strcmp", "strcpy", "strcat", "strlen", "strchr", "strstr", "strncpy",
    "strtol", "strtod", "atoi", "atof",
    "setjmp", "longjmp", "signal", "perror", "remove", "rename",
    "clock", "va_start", "va_arg", "va_end", "creal", "cimag", "qsort", "bsearch"
};

static int is_builtin(const char *name, int *params, const char **type) {
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(BUILTINS[i].name, name) == 0) {
            if (params) *params = BUILTINS[i].params;
            if (type) *type = BUILTINS[i].type;
            return 1;
        }
    }
    return 0;
}

static int is_unsupported_fn(const char *name) {
    for (size_t i = 0; i < sizeof(UNSUPPORTED_FNS) / sizeof(UNSUPPORTED_FNS[0]); i++) {
        if (strcmp(UNSUPPORTED_FNS[i], name) == 0) return 1;
    }
    return 0;
}

/* ------------------------------ list helpers ----------------------------- */

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }

static void recrec_init(SymRecList *l) { l->count = 0; l->capacity = 8; l->items = (SymRec *)xmalloc(sizeof(SymRec) * 8); }
static SymRec *recrec_add(SymRecList *l) {
    if (l->count >= l->capacity) { l->capacity *= 2; l->items = (SymRec *)xrealloc(l->items, sizeof(SymRec) * (size_t)l->capacity); }
    SymRec *r = &l->items[l->count++];
    memset(r, 0, sizeof(SymRec));
    return r;
}
static SymRec *recrec_find(SymRecList *l, const char *name) {
    for (int i = 0; i < l->count; i++) if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
    return NULL;
}

static void push_symbol(SemResult *s, const char *scope, const char *name, const char *kind,
                        const char *type, int offset, int params) {
    if (s->symbol_count >= s->symbol_capacity) {
        s->symbol_capacity = s->symbol_capacity ? s->symbol_capacity * 2 : 32;
        s->symbols = (SymRow *)xrealloc(s->symbols, sizeof(SymRow) * (size_t)s->symbol_capacity);
    }
    SymRow *row = &s->symbols[s->symbol_count++];
    memset(row, 0, sizeof(SymRow));
    strncpy(row->scope, scope, sizeof(row->scope) - 1);
    strncpy(row->name, name, sizeof(row->name) - 1);
    strncpy(row->kind, kind, sizeof(row->kind) - 1);
    strncpy(row->type, type, sizeof(row->type) - 1);
    snprintf(row->address, sizeof(row->address), "0x%04X", (unsigned)(offset * 4));
    row->params = params;
}

static FuncDef *push_function(SemResult *s) {
    if (s->func_count >= s->func_capacity) {
        s->func_capacity = s->func_capacity ? s->func_capacity * 2 : 8;
        s->functions = (FuncDef *)xrealloc(s->functions, sizeof(FuncDef) * (size_t)s->func_capacity);
    }
    FuncDef *f = &s->functions[s->func_count++];
    memset(f, 0, sizeof(FuncDef));
    recrec_init(&f->frame);
    return f;
}

SymRec *sem_find_global(SemResult *s, const char *name) { return recrec_find(&s->globals, name); }
static SymRec *sem_find_in_frame_last(FuncDef *f, const char *name);
FuncDef *sem_find_function(SemResult *s, const char *name) {
    for (int i = 0; i < s->func_count; i++) if (strcmp(s->functions[i].name, name) == 0) return &s->functions[i];
    return NULL;
}
StructDef *sem_find_struct(SemResult *s, const char *name) {
    for (int i = 0; i < s->struct_count; i++) if (strcmp(s->structs[i].name, name) == 0) return &s->structs[i];
    return NULL;
}
SymRec *sem_find_in_frame(FuncDef *f, const char *name) {
    /* Return the LAST declaration with this name: matches the JS engine's Map
     * semantics where a later same-name declaration shadows the earlier one. */
    return sem_find_in_frame_last(f, name);
}

static int type_size(SemResult *s, const char *typeName) {
    if (strncmp(typeName, "struct ", 7) == 0) {
        StructDef *st = sem_find_struct(s, typeName + 7);
        return st ? st->size : 1;
    }
    return 1;
}

static long long truncate_ll(double v) {
    const double LIM = 9007199254740991.0;
    if (v >= LIM) return 9007199254740991LL;
    if (v <= -LIM) return -9007199254740991LL;
    return (long long)v;
}

/* ------------------------------ main entry ------------------------------- */

static void sem_walk_collect(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node);
static void sem_check_stmt(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node, int inLoop, int inSwitch);

SemResult *nova_semantic(NovaNode *ast, DiagList *diags) {
    SemResult *s = (SemResult *)xcalloc(1, sizeof(SemResult));
    s->diags = diags;
    recrec_init(&s->globals);

    /* builtins first */
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        push_symbol(s, "global", BUILTINS[i].name, "Function", BUILTINS[i].type, 0, BUILTINS[i].params);
    }

    /* errno builtin global */
    {
        SymRec *rec = recrec_add(&s->globals);
        strcpy(rec->name, "errno");
        strcpy(rec->type, "int");
        rec->size = 1;
        rec->isGlobal = 1;
        rec->offset = s->globalSlotCount;
        s->globalSlotCount += 1;
        push_symbol(s, "global", "errno", "Variable", "int", 0, 0);
    }

    /* Pass 1a: structs */
    for (int i = 0; i < ast->child_count; i++) {
        NovaNode *node = ast->children[i];
        if (node->type != NODE_STRUCT_DEF) continue;
        if (sem_find_struct(s, node->identifier)) {
            diag_add(diags, "error", node->line, 1, "Redefinition of struct '%s'", node->identifier);
            continue;
        }
        if (s->struct_count >= s->struct_capacity) {
            s->struct_capacity = s->struct_capacity ? s->struct_capacity * 2 : 8;
            s->structs = (StructDef *)xrealloc(s->structs, sizeof(StructDef) * (size_t)s->struct_capacity);
        }
        StructDef *st = &s->structs[s->struct_count++];
        memset(st, 0, sizeof(StructDef));
        strncpy(st->name, node->identifier, sizeof(st->name) - 1);
        st->field_capacity = 8;
        st->fields = (StructField *)xmalloc(sizeof(StructField) * 8);
        int offset = 0;
        for (int f = 0; f < node->child_count; f++) {
            NovaNode *fn = node->children[f];
            if (st->field_count >= st->field_capacity) {
                st->field_capacity *= 2;
                st->fields = (StructField *)xrealloc(st->fields, sizeof(StructField) * (size_t)st->field_capacity);
            }
            StructField *fld = &st->fields[st->field_count++];
            memset(fld, 0, sizeof(StructField));
            strncpy(fld->name, fn->identifier, sizeof(fld->name) - 1);
            strncpy(fld->type, fn->type_name, sizeof(fld->type) - 1);
            fld->is_array = fn->is_array;
            fld->size = fn->is_array
                ? (fn->has_size ? (truncate_ll(fn->children[0]->num_val) > 1 ? (int)truncate_ll(fn->children[0]->num_val) : 1) : 1)
                : type_size(s, fn->type_name);
            fld->offset = offset;
            offset += fld->size;
        }
        st->size = offset;
        push_symbol(s, "global", node->identifier, "Struct", "struct", 0, st->field_count);
    }

    /* Pass 1b: functions & globals */
    for (int i = 0; i < ast->child_count; i++) {
        NovaNode *raw = ast->children[i];
        NovaNode **declNodes;
        int declCount;
        if (raw->type == NODE_DECL_LIST) { declNodes = raw->children; declCount = raw->child_count; }
        else { declNodes = &raw; declCount = 1; }
        for (int d = 0; d < declCount; d++) {
            NovaNode *node = declNodes[d];
            if (node->type == NODE_FUNCTION_DEF) {
                FuncDef *existing = sem_find_function(s, node->identifier);
                if (existing) {
                    if (existing->is_forward && !node->is_forward) {
                        /* real definition replaces forward declaration */
                        existing->node = node;
                        existing->is_forward = 0;
                        strncpy(existing->type_name, node->type_name, sizeof(existing->type_name) - 1);
                        /* rebuild param list */
                        existing->frame.count = 0;
                        existing->param_count = 0;
                        for (int c = 0; c < node->child_count; c++) {
                            if (node->children[c]->type == NODE_PARAMETER) existing->param_count++;
                        }
                        continue;
                    }
                    diag_add(diags, "error", node->line, 1, "Redefinition of function '%s'", node->identifier);
                    continue;
                }
                FuncDef *f = push_function(s);
                strncpy(f->name, node->identifier, sizeof(f->name) - 1);
                strncpy(f->type_name, node->type_name, sizeof(f->type_name) - 1);
                f->node = node;
                f->is_forward = node->is_forward;
                for (int c = 0; c < node->child_count; c++) {
                    if (node->children[c]->type == NODE_PARAMETER) f->param_count++;
                }
                push_symbol(s, "global", node->identifier, "Function", node->type_name, 0, f->param_count);
            } else if (node->type == NODE_VAR_DECL) {
                if (sem_find_global(s, node->identifier)) {
                    diag_add(diags, "error", node->line, 1, "Redefinition of global '%s'", node->identifier);
                    continue;
                }
                int size = node->is_array
                    ? (node->has_size ? (truncate_ll(node->children[0]->num_val) > 1 ? (int)truncate_ll(node->children[0]->num_val) : 1)
                       : (node->child_count > 1 ? node->child_count
                          : (node->child_count == 1 && node->children[0]->type == NODE_STRING_LITERAL
                             ? (int)strlen(node->children[0]->string_val) + 1 : 1)))
                    : type_size(s, node->type_name);
                SymRec *rec = recrec_add(&s->globals);
                strncpy(rec->name, node->identifier, sizeof(rec->name) - 1);
                strncpy(rec->type, node->type_name, sizeof(rec->type) - 1);
                rec->is_array = node->is_array;
                rec->size = size;
                rec->isGlobal = 1;
                rec->offset = s->globalSlotCount;
                s->globalSlotCount += size;
                push_symbol(s, "global", node->identifier, node->is_array ? "Array" : "Variable",
                            node->type_name, rec->offset, 0);
            }
        }
    }

    if (!sem_find_function(s, "main")) {
        diag_add(diags, "error", 1, 1, "No 'main' function defined");
    }

    /* Pass 2: function bodies — assign frame slots, collect statics */
    for (int fi = 0; fi < s->func_count; fi++) {
        FuncDef *f = &s->functions[fi];
        /* params */
        for (int c = 0; f->node && c < f->node->child_count; c++) {
            NovaNode *pn = f->node->children[c];
            if (pn->type != NODE_PARAMETER) continue;
            SymRec *rec = recrec_add(&f->frame);
            strncpy(rec->name, pn->identifier, sizeof(rec->name) - 1);
            strncpy(rec->type, pn->type_name, sizeof(rec->type) - 1);
            rec->size = 1;
            rec->is_array = pn->is_array;
            rec->isParam = 1;
            rec->offset = f->frame_size;
            f->frame_size += 1;
            push_symbol(s, f->name, pn->identifier, "Parameter", pn->type_name, rec->offset, 0);
        }

        NovaNode *body = NULL;
        for (int c = 0; f->node && c < f->node->child_count; c++) {
            if (f->node->children[c]->type != NODE_PARAMETER) { body = f->node->children[c]; break; }
        }
        sem_walk_collect(s, f, diags, body);
    }

    /* Pass 3: expression & statement checks (mirrors the JS walkExpr/walkStmt) */
    for (int fi = 0; fi < s->func_count; fi++) {
        FuncDef *f = &s->functions[fi];
        if (f->is_forward) continue;
        NovaNode *body = NULL;
        for (int c = 0; f->node && c < f->node->child_count; c++) {
            if (f->node->children[c]->type != NODE_PARAMETER) { body = f->node->children[c]; break; }
        }
        sem_check_stmt(s, f, diags, body, 0, 0);
    }

    return s;
}

/* Collect local declarations (recursively), assigning frame slots. Static
 * locals are promoted to global storage and recorded for one-time init. */
/* A scope frame holds the names declared in one lexical scope so the
 * duplicate check is per-scope (matching the JS engine, which pushes a fresh
 * scope for for-loop inits and compound statements). Sibling for-loops may
 * each declare their own 'i'. */
typedef struct ScopeNames {
    char (*names)[256];
    int count, capacity;
    struct ScopeNames *parent;
} ScopeNames;

static void scope_push_name(ScopeNames *s, const char *name) {
    if (s->count >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 16;
        s->names = (char (*)[256])xrealloc(s->names, sizeof(char[256]) * (size_t)s->capacity);
    }
    strncpy(s->names[s->count], name, 255);
    s->names[s->count][255] = '\0';
    s->count++;
}
static int scope_has(ScopeNames *s, const char *name) {
    for (int i = 0; i < s->count; i++) if (strcmp(s->names[i], name) == 0) return 1;
    return 0;
}

/* Find the LAST declaration with a name in the frame (matches the JS engine's
 * Map semantics where a later same-name declaration shadows the earlier one). */
static SymRec *sem_find_in_frame_last(FuncDef *f, const char *name) {
    if (!f) return NULL;
    for (int i = f->frame.count - 1; i >= 0; i--) {
        if (strcmp(f->frame.items[i].name, name) == 0) return &f->frame.items[i];
    }
    return NULL;
}

static void sem_declare_local(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node, ScopeNames *scope) {
    if (scope_has(scope, node->identifier)) {
        diag_add(diags, "error", node->line, 1, "Duplicate declaration of '%s'", node->identifier);
        return;
    }
    scope_push_name(scope, node->identifier);
    int size = node->is_array
        ? (node->has_size ? (truncate_ll(node->children[0]->num_val) > 1 ? (int)truncate_ll(node->children[0]->num_val) : 1)
           : (node->child_count > 1 ? node->child_count
              : (node->child_count == 1 && node->children[0]->type == NODE_STRING_LITERAL
                 ? (int)strlen(node->children[0]->string_val) + 1 : 1)))
        : type_size(ss, node->type_name);
    if (node->is_static) {
        if (sem_find_global(ss, node->identifier)) {
            diag_add(diags, "error", node->line, 1, "Redefinition of global '%s'", node->identifier);
            return;
        }
        SymRec *grec = recrec_add(&ss->globals);
        strncpy(grec->name, node->identifier, sizeof(grec->name) - 1);
        strncpy(grec->type, node->type_name, sizeof(grec->type) - 1);
        grec->is_array = node->is_array;
        grec->size = size;
        grec->isGlobal = 1;
        grec->offset = ss->globalSlotCount;
        ss->globalSlotCount += size;
        SymRec *frec = recrec_add(&ff->frame);
        *frec = *grec;
        push_symbol(ss, ff->name, node->identifier, node->is_array ? "Array" : "Variable (static)",
                    node->type_name, grec->offset, 0);
        if (ss->static_count >= ss->static_capacity) {
            ss->static_capacity = ss->static_capacity ? ss->static_capacity * 2 : 8;
            ss->staticDecls = (NovaNode **)xrealloc(ss->staticDecls, sizeof(NovaNode *) * (size_t)ss->static_capacity);
        }
        ss->staticDecls[ss->static_count++] = node;
        return;
    }
    SymRec *rec = recrec_add(&ff->frame);
    strncpy(rec->name, node->identifier, sizeof(rec->name) - 1);
    strncpy(rec->type, node->type_name, sizeof(rec->type) - 1);
    rec->is_array = node->is_array;
    rec->size = size;
    rec->offset = ff->frame_size;
    ff->frame_size += size;
    push_symbol(ss, ff->name, node->identifier, node->is_array ? "Array" : "Variable",
                node->type_name, rec->offset, 0);
}

static void sem_walk_collect_scoped(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node, ScopeNames *scope) {
    if (!node) return;
    if (node->type == NODE_VAR_DECL) { sem_declare_local(ss, ff, diags, node, scope); return; }
    if (node->type == NODE_COMPOUND_STMT) {
        ScopeNames child; memset(&child, 0, sizeof(child)); child.parent = scope;
        for (int i = 0; i < node->child_count; i++) sem_walk_collect_scoped(ss, ff, diags, node->children[i], &child);
        free(child.names);
        return;
    }
    if (node->type == NODE_FOR_STMT) {
        /* for-loop init gets its own scope (like the JS engine) */
        ScopeNames child; memset(&child, 0, sizeof(child)); child.parent = scope;
        for (int i = 0; i < node->child_count; i++) sem_walk_collect_scoped(ss, ff, diags, node->children[i], &child);
        free(child.names);
        return;
    }
    for (int i = 0; i < node->child_count; i++) sem_walk_collect_scoped(ss, ff, diags, node->children[i], scope);
}

static void sem_walk_collect(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node) {
    ScopeNames root; memset(&root, 0, sizeof(root));
    /* params are already in the frame; register them in the root scope so a
     * local re-declaring a parameter name is flagged as duplicate */
    for (int i = 0; i < ff->frame.count; i++) scope_push_name(&root, ff->frame.items[i].name);
    sem_walk_collect_scoped(ss, ff, diags, node, &root);
    free(root.names);
}

/* ------------------------------------------------------------------------- */
/* Expression & statement checks (Pass 3)                                     */
/* ------------------------------------------------------------------------- */

static int sem_resolve(SemResult *ss, FuncDef *ff, const char *name, int line, DiagList *diags) {
    /* returns 1 if resolved */
    if (ff && sem_find_in_frame(ff, name)) return 1;
    if (sem_find_global(ss, name)) return 1;
    if (sem_find_function(ss, name)) return 1;
    int bp; const char *bt;
    if (is_builtin(name, &bp, &bt)) return 1;
    diag_add(diags, "error", line, 1, "Undefined identifier '%s'", name);
    return 0;
}

static void sem_check_assignable(DiagList *diags, NovaNode *node) {
    if (!node) return;
    if (node->type == NODE_IDENTIFIER || node->type == NODE_INDEX || node->type == NODE_MEMBER) return;
    if (node->type == NODE_UNARY_OP && strcmp(node->op, "*") == 0) return;
    diag_add(diags, "error", node->line, 1, "Assignment to non-lvalue");
}

static void sem_check_expr(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node) {
    if (!node) return;
    if (node->type == NODE_IDENTIFIER) {
        sem_resolve(ss, ff, node->identifier, node->line, diags);
        return;
    }
    if (node->type == NODE_ASSIGNMENT || node->type == NODE_COMPOUND_ASSIGN) {
        sem_check_assignable(diags, node->child_count > 0 ? node->children[0] : NULL);
        for (int i = 0; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
        return;
    }
    if (node->type == NODE_FUNC_CALL) {
        int bp; const char *bt;
        if (is_builtin(node->identifier, &bp, &bt)) {
            if (bp >= 0 && node->child_count != bp) {
                diag_add(diags, "error", node->line, 1,
                         "Function '%s' expects %d argument(s), got %d", node->identifier, bp, node->child_count);
            }
        } else if (is_unsupported_fn(node->identifier)) {
            diag_add(diags, "error", node->line, 1,
                     "Function '%s' is not supported in the NOVA C subset (no libc/heap/IO in the educational VM)",
                     node->identifier);
        } else {
            FuncDef *f = sem_find_function(ss, node->identifier);
            if (!f) {
                /* only report undefined if not resolved as a variable/global */
                if (!(ff && sem_find_in_frame(ff, node->identifier)) && !sem_find_global(ss, node->identifier)) {
                    diag_add(diags, "error", node->line, 1, "Undefined identifier '%s'", node->identifier);
                }
            } else if (f->param_count != node->child_count) {
                diag_add(diags, "error", node->line, 1,
                         "Function '%s' expects %d argument(s), got %d", node->identifier, f->param_count, node->child_count);
            }
        }
        for (int i = 0; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
        return;
    }
    for (int i = 0; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
}

static void sem_check_stmt(SemResult *ss, FuncDef *ff, DiagList *diags, NovaNode *node, int inLoop, int inSwitch) {
    if (!node) return;
    switch (node->type) {
        case NODE_DECL_LIST:
            for (int i = 0; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, inSwitch);
            return;
        case NODE_VAR_DECL:
            /* initializer expressions */
            {
                int start = node->is_array && node->has_size ? 1 : 0;
                for (int i = start; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
            }
            return;
        case NODE_COMPOUND_STMT:
            for (int i = 0; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, inSwitch);
            return;
        case NODE_IF_STMT:
            sem_check_expr(ss, ff, diags, node->children[0]);
            sem_check_stmt(ss, ff, diags, node->children[1], inLoop, inSwitch);
            if (node->child_count > 2) sem_check_stmt(ss, ff, diags, node->children[2], inLoop, inSwitch);
            return;
        case NODE_WHILE_STMT:
            sem_check_expr(ss, ff, diags, node->children[0]);
            sem_check_stmt(ss, ff, diags, node->children[1], 1, inSwitch);
            return;
        case NODE_DO_WHILE_STMT:
            sem_check_stmt(ss, ff, diags, node->children[0], 1, inSwitch);
            sem_check_expr(ss, ff, diags, node->children[1]);
            return;
        case NODE_SWITCH_STMT:
            sem_check_expr(ss, ff, diags, node->children[0]);
            for (int i = 1; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, 1);
            return;
        case NODE_CASE:
        case NODE_DEFAULT:
            for (int i = 0; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, 1);
            return;
        case NODE_FOR_STMT:
            sem_check_stmt(ss, ff, diags, node->children[0], 1, inSwitch);
            sem_check_expr(ss, ff, diags, node->children[1]);
            sem_check_expr(ss, ff, diags, node->children[2]);
            sem_check_stmt(ss, ff, diags, node->children[3], 1, inSwitch);
            return;
        case NODE_LABEL_STMT:
            for (int i = 0; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, inSwitch);
            return;
        case NODE_BREAK_STMT:
            if (!inLoop && !inSwitch) diag_add(diags, "error", node->line, 1, "'break' used outside of a loop or switch");
            return;
        case NODE_CONTINUE_STMT:
            if (!inLoop) diag_add(diags, "error", node->line, 1, "'continue' used outside of a loop");
            return;
        case NODE_RETURN_STMT:
            for (int i = 0; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
            return;
        case NODE_EXPRESSION_STMT:
            for (int i = 0; i < node->child_count; i++) sem_check_expr(ss, ff, diags, node->children[i]);
            return;
        default:
            for (int i = 0; i < node->child_count; i++) sem_check_stmt(ss, ff, diags, node->children[i], inLoop, inSwitch);
    }
}

void nova_semantic_free(SemResult *s) {
    if (!s) return;
    free(s->symbols);
    free(s->globals.items);
    for (int i = 0; i < s->func_count; i++) free(s->functions[i].frame.items);
    free(s->functions);
    for (int i = 0; i < s->struct_count; i++) free(s->structs[i].fields);
    free(s->structs);
    free(s->staticDecls);
    free(s);
}

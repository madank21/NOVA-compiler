#include "tac.h"
#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Containers                                                                 */
/* ------------------------------------------------------------------------- */

static void* xmalloc(size_t n) { void* p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void* xrealloc(void* p, size_t n) { void* q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }
static void* xcalloc(size_t n, size_t s) { void* p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }

static TACList* taclist_new(void) {
    TACList* l = (TACList*)xcalloc(1, sizeof(TACList));
    l->capacity = 64;
    l->items = (TACInstr*)xmalloc(sizeof(TACInstr) * (size_t)l->capacity);
    return l;
}

void tac_list_free(TACList* list) {
    if (!list) return;
    free(list->items);
    free(list);
}

TACList* tac_list_clone(const TACList* list) {
    TACList* out = taclist_new();
    for (int i = 0; i < list->count; i++) {
        if (out->count >= out->capacity) {
            out->capacity *= 2;
            out->items = (TACInstr*)xrealloc(out->items, sizeof(TACInstr) * (size_t)out->capacity);
        }
        out->items[out->count++] = list->items[i];
    }
    return out;
}

static void templist_init(TempTypeList* l) {
    l->capacity = 32; l->count = 0;
    l->items = (TempType*)xmalloc(sizeof(TempType) * (size_t)l->capacity);
}

static void templist_add(TempTypeList* l, const char* name, const char* type) {
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->items = (TempType*)xrealloc(l->items, sizeof(TempType) * (size_t)l->capacity);
    }
    TempType* t = &l->items[l->count++];
    memset(t, 0, sizeof(TempType));
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->type, sizeof(t->type), "%s", type);
}

const char* temp_type_of(const TempTypeList* l, const char* name) {
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) return l->items[i].type;
    }
    return NULL;
}

static void strlist_init(StrList* l) {
    l->capacity = 16; l->count = 0;
    l->items = (char**)xmalloc(sizeof(char*) * (size_t)l->capacity);
}

static void strlist_push(StrList* l, char* s) {
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->items = (char**)xrealloc(l->items, sizeof(char*) * (size_t)l->capacity);
    }
    l->items[l->count++] = s;
}

/* ------------------------------------------------------------------------- */
/* Generator                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char place[64];
    char type[64];
    int is_const;
} Place;

typedef struct {
    SemModel* sem;
    DiagList* diags;
    StrPool* pool;
    TACList* instrs;
    TempTypeList temp_types;
    StrList strings;
    int temp_count;
    int label_count;
    /* loop stack */
    char loop_brk[32][16];
    char loop_cont[32][16];
    int loop_depth;
} Gen;

static void emit(Gen* g, const char* op, const char* res, const char* a1, const char* a2, int line) {
    TACList* l = g->instrs;
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->items = (TACInstr*)xrealloc(l->items, sizeof(TACInstr) * (size_t)l->capacity);
    }
    TACInstr* ins = &l->items[l->count++];
    memset(ins, 0, sizeof(TACInstr));
    snprintf(ins->op, sizeof(ins->op), "%s", op);
    if (res) snprintf(ins->res, sizeof(ins->res), "%s", res);
    if (a1) snprintf(ins->a1, sizeof(ins->a1), "%s", a1);
    if (a2) snprintf(ins->a2, sizeof(ins->a2), "%s", a2);
    ins->line = line;
}

static void new_temp(Gen* g, const char* type, char out[64]) {
    snprintf(out, 64, "t%d", g->temp_count++);
    if (type) templist_add(&g->temp_types, out, type);
}

static void new_label(Gen* g, char out[16]) {
    snprintf(out, 16, "L%d", g->label_count++);
}

static int is_float_type(const char* t) {
    return strcmp(t, "float") == 0 || strcmp(t, "double") == 0;
}

static int intern_string(Gen* g, const char* s) {
    strlist_push(&g->strings, strpool_dup(g->pool, s ? s : ""));
    return g->strings.count - 1;
}

/* find a symbol record anywhere (globals then all frames) */
static SymRec* find_any_symbol(Gen* g, const char* name) {
    SymRec* r = sem_find_global(g->sem, name);
    if (r) return r;
    for (int i = 0; i < g->sem->func_count; i++) {
        r = reclist_find_pub(&g->sem->functions[i].frame, name);
        if (r) return r;
    }
    return NULL;
}

static const char* sem_expr_type(Gen* g, ASTNode* node) {
    if (strcmp(node->node_type, "NODE_IDENTIFIER") != 0) return "int";
    SymRec* rec = find_any_symbol(g, node->identifier);
    return rec ? rec->type : "int";
}

static void const_string(double v, const char* type, char out[64]) {
    if (is_float_type(type)) {
        format_float_const(v, out, 64);
    } else {
        snprintf(out, 64, "%lld", nova_truncate_i64(v));
    }
}

/* forward decls */
static Place gen_expr(Gen* g, ASTNode* node);
static void gen_stmt(Gen* g, ASTNode* node);
static void gen_var_decl(Gen* g, ASTNode* node);

static Place place_const_int(long long v) {
    Place p; memset(&p, 0, sizeof(p));
    snprintf(p.place, sizeof(p.place), "%lld", v);
    strcpy(p.type, "int");
    p.is_const = 1;
    return p;
}

static int place_of_literal(Gen* g, ASTNode* node, Place* out) {
    (void)g;
    if (strcmp(node->node_type, "NODE_INT_LITERAL") == 0) {
        *out = place_const_int(nova_truncate_i64(node->num_val));
        return 1;
    }
    if (strcmp(node->node_type, "NODE_FLOAT_LITERAL") == 0) {
        Place p; memset(&p, 0, sizeof(p));
        const_string(node->num_val, "double", p.place);
        strcpy(p.type, "double");
        p.is_const = 1;
        *out = p;
        return 1;
    }
    return 0;
}

static void gen_index_addr(Gen* g, ASTNode* node, char out[64]) {
    ASTNode* base = node->children[0];
    Place idx = gen_expr(g, node->children[1]);
    char t[64];
    new_temp(g, "ptr", t);
    char base_name[64];
    if (strcmp(base->node_type, "NODE_IDENTIFIER") == 0) snprintf(base_name, sizeof(base_name), "%.*s", (int)(sizeof(base_name) - 1), base->identifier);
    else strcpy(base_name, "0");
    base_name[63] = '\0';
    emit(g, "IDX_ADDR", t, base_name, idx.place, node->line);
    strcpy(out, t);
}

/* returns 1 on success, fills out + field_type */
static int gen_member_addr(Gen* g, ASTNode* node, char out[64], char field_type[64]) {
    ASTNode* base = node->child_count > 0 ? node->children[0] : NULL;
    if (base && strcmp(base->node_type, "NODE_IDENTIFIER") == 0) {
        SymRec* rec = find_any_symbol(g, base->identifier);
        if (rec && strncmp(rec->type, "struct ", 7) == 0) {
            StructDef* s = sem_get_struct(g->sem, rec->type + 7);
            if (s) {
                for (int i = 0; i < s->field_count; i++) {
                    if (strcmp(s->fields[i].name, node->identifier) == 0) {
                        char t[64];
                        new_temp(g, "ptr", t);
                        char off[32];
                        snprintf(off, sizeof(off), "%d", s->fields[i].offset);
                        emit(g, "ADDR", t, base->identifier, off, node->line);
                        strcpy(out, t);
                        if (s->fields[i].is_array) {
                            /* bound the base so "*\0" always fits in 64 bytes */
                            snprintf(field_type, 64, "%.*s*",
                                     (int)(sizeof(s->fields[i].type) - 2), s->fields[i].type);
                        } else {
                            snprintf(field_type, 64, "%s", s->fields[i].type);
                        }
                        return 1;
                    }
                }
            }
        }
    }
    diag_add(g->diags, "error", node->line, 1, "Invalid member access");
    return 0;
}

static void gen_call(Gen* g, ASTNode* node, Place* out) {
    int is_io = strcmp(node->identifier, "printf") == 0 || strcmp(node->identifier, "scanf") == 0;
    if (is_io) {
        ASTNode* first = node->child_count > 0 ? node->children[0] : NULL;
        const char* fmt = "";
        if (first && strcmp(first->node_type, "NODE_STRING_LITERAL") == 0) {
            fmt = first->string_val;
        } else if (first) {
            diag_add(g->diags, "error", node->line, 1, "%s format must be a string literal", node->identifier);
        }
        int idx = intern_string(g, fmt);
        for (int i = 1; i < node->child_count; i++) {
            Place a = gen_expr(g, node->children[i]);
            emit(g, "PARAM", "", a.place, "", node->line);
        }
        char strref[32];
        snprintf(strref, sizeof(strref), "\"str%d\"", idx);
        char nargs[16];
        snprintf(nargs, sizeof(nargs), "%d", node->child_count - 1);
        emit(g, strcmp(node->identifier, "printf") == 0 ? "PRINT" : "READ",
             node->identifier, strref, nargs, node->line);
        memset(out, 0, sizeof(Place));
        strcpy(out->type, "int");
        return;
    }
    for (int i = 0; i < node->child_count; i++) {
        Place a = gen_expr(g, node->children[i]);
        emit(g, "PARAM", "", a.place, "", node->line);
    }
    char t[64];
    new_temp(g, "int", t);
    char nargs[16];
    snprintf(nargs, sizeof(nargs), "%d", node->child_count);
    emit(g, "CALL", t, node->identifier, nargs, node->line);
    memset(out, 0, sizeof(Place));
    snprintf(out->place, sizeof(out->place), "%s", t);
    strcpy(out->type, "int");
}

/* lvalue addressing */
typedef struct {
    int valid;
    int direct;         /* 1 -> name; 0 -> addr place */
    char name[64];
    char place[64];
    int line;
} LValue;

static LValue lvalue_addr(Gen* g, ASTNode* node) {
    LValue lv; memset(&lv, 0, sizeof(lv));
    lv.line = node->line;
    if (strcmp(node->node_type, "NODE_IDENTIFIER") == 0) {
        lv.valid = 1; lv.direct = 1;
        snprintf(lv.name, sizeof(lv.name), "%.*s", (int)(sizeof(lv.name) - 1), node->identifier);
        return lv;
    }
    if (strcmp(node->node_type, "NODE_INDEX") == 0) {
        lv.valid = 1; lv.direct = 0;
        gen_index_addr(g, node, lv.place);
        return lv;
    }
    if (strcmp(node->node_type, "NODE_MEMBER") == 0) {
        char ft[64];
        if (gen_member_addr(g, node, lv.place, ft)) { lv.valid = 1; lv.direct = 0; }
        return lv;
    }
    if (strcmp(node->node_type, "NODE_UNARY_OP") == 0 && strcmp(node->op, "*") == 0) {
        Place p = gen_expr(g, node->children[0]);
        lv.valid = 1; lv.direct = 0;
        snprintf(lv.place, sizeof(lv.place), "%s", p.place);
        return lv;
    }
    return lv;
}

static void store_to_lvalue(Gen* g, const LValue* lv, const char* value_place, int line) {
    if (!lv->valid) return;
    if (lv->direct) emit(g, "=", lv->name, value_place, "", line);
    else emit(g, "STORE_PTR", "", lv->place, value_place, line);
}

static void gen_assignment(Gen* g, ASTNode* node, Place* out) {
    ASTNode* target = node->children[0];
    if (strcmp(node->node_type, "NODE_ASSIGNMENT") == 0) {
        Place rhs = gen_expr(g, node->children[1]);
        LValue lv = lvalue_addr(g, target);
        store_to_lvalue(g, &lv, rhs.place, node->line);
        *out = rhs;
        return;
    }
    const char* op = NULL;
    if (strcmp(node->op, "+=") == 0) op = "+";
    else if (strcmp(node->op, "-=") == 0) op = "-";
    else if (strcmp(node->op, "*=") == 0) op = "*";
    else if (strcmp(node->op, "/=") == 0) op = "/";
    else if (strcmp(node->op, "%=") == 0) op = "%";
    LValue lv = lvalue_addr(g, target);
    if (!lv.valid) { memset(out, 0, sizeof(Place)); strcpy(out->type, "int"); return; }

    Place oldv; memset(&oldv, 0, sizeof(oldv));
    if (lv.direct) {
        snprintf(oldv.place, sizeof(oldv.place), "%s", lv.name);
        snprintf(oldv.type, sizeof(oldv.type), "%s", sem_expr_type(g, target));
    } else {
        char t[64]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, lv.place, "", node->line);
        snprintf(oldv.place, sizeof(oldv.place), "%s", t);
        strcpy(oldv.type, "int");
    }
    Place rhs = gen_expr(g, node->children[1]);
    const char* res_type = (is_float_type(oldv.type) || is_float_type(rhs.type)) ? "double" : "int";
    char t[64]; new_temp(g, res_type, t);
    emit(g, op, t, oldv.place, rhs.place, node->line);
    store_to_lvalue(g, &lv, t, node->line);
    memset(out, 0, sizeof(Place));
    snprintf(out->place, sizeof(out->place), "%s", t);
    snprintf(out->type, sizeof(out->type), "%s", res_type);
}

static void gen_inc_dec(Gen* g, ASTNode* node, Place* out) {
    const char* op = (strcmp(node->op, "++") == 0 || strcmp(node->op, "p++") == 0) ? "+" : "-";
    int prefix = (strcmp(node->op, "++") == 0 || strcmp(node->op, "--") == 0);
    ASTNode* target = node->children[0];
    LValue lv = lvalue_addr(g, target);
    if (!lv.valid) { memset(out, 0, sizeof(Place)); strcpy(out->type, "int"); out->is_const = 1; return; }

    Place oldv; memset(&oldv, 0, sizeof(oldv));
    if (lv.direct) {
        snprintf(oldv.place, sizeof(oldv.place), "%s", lv.name);
        snprintf(oldv.type, sizeof(oldv.type), "%s", sem_expr_type(g, target));
    } else {
        char t[64]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, lv.place, "", node->line);
        snprintf(oldv.place, sizeof(oldv.place), "%s", t);
        strcpy(oldv.type, "int");
    }
    char t[64];
    new_temp(g, strcmp(oldv.type, "double") == 0 ? "double" : "int", t);
    emit(g, op, t, oldv.place, "1", node->line);
    store_to_lvalue(g, &lv, t, node->line);
    if (prefix) {
        memset(out, 0, sizeof(Place));
        snprintf(out->place, sizeof(out->place), "%s", t);
        strcpy(out->type, strcmp(oldv.type, "double") == 0 ? "double" : "int");
    } else {
        *out = oldv;
    }
}

static Place gen_expr(Gen* g, ASTNode* node) {
    Place zero; memset(&zero, 0, sizeof(zero));
    strcpy(zero.place, "0"); strcpy(zero.type, "int"); zero.is_const = 1;
    if (!node || strcmp(node->node_type, "NODE_ERROR") == 0) return zero;

    Place lit;
    if (place_of_literal(g, node, &lit)) return lit;

    if (strcmp(node->node_type, "NODE_STRING_LITERAL") == 0) {
        int idx = intern_string(g, node->string_val);
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "\"str%d\"", idx);
        strcpy(p.type, "char*");
        p.is_const = 1;
        return p;
    }

    if (strcmp(node->node_type, "NODE_IDENTIFIER") == 0) {
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "%.*s", (int)(sizeof(p.place) - 1), node->identifier);
        snprintf(p.type, sizeof(p.type), "%s", sem_expr_type(g, node));
        return p;
    }

    if (strcmp(node->node_type, "NODE_BINARY_OP") == 0) {
        const char* op = node->op;
        if (strcmp(op, "&&") == 0) {
            Place a = gen_expr(g, node->children[0]);
            char t[64]; new_temp(g, "int", t);
            char lshort[16]; new_label(g, lshort);
            emit(g, "=", t, a.place, "", node->line);
            emit(g, "IF_FALSE", lshort, t, "", node->line);
            Place bv = gen_expr(g, node->children[1]);
            emit(g, "=", t, bv.place, "", node->line);
            emit(g, "LABEL", lshort, "", "", node->line);
            emit(g, "!=", t, t, "0", node->line);
            Place p; memset(&p, 0, sizeof(p));
            snprintf(p.place, sizeof(p.place), "%s", t); strcpy(p.type, "int");
            return p;
        }
        if (strcmp(op, "||") == 0) {
            Place a = gen_expr(g, node->children[0]);
            char t[64]; new_temp(g, "int", t);
            char lcont[16], lnorm[16];
            new_label(g, lcont); new_label(g, lnorm);
            emit(g, "=", t, a.place, "", node->line);
            emit(g, "IF_FALSE", lcont, t, "", node->line);
            emit(g, "GOTO", lnorm, "", "", node->line);
            emit(g, "LABEL", lcont, "", "", node->line);
            Place bv = gen_expr(g, node->children[1]);
            emit(g, "=", t, bv.place, "", node->line);
            emit(g, "LABEL", lnorm, "", "", node->line);
            emit(g, "!=", t, t, "0", node->line);
            Place p; memset(&p, 0, sizeof(p));
            snprintf(p.place, sizeof(p.place), "%s", t); strcpy(p.type, "int");
            return p;
        }
        Place l = gen_expr(g, node->children[0]);
        Place r = gen_expr(g, node->children[1]);
        const char* res_type;
        if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
            strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) res_type = "int";
        else if (is_float_type(l.type) || is_float_type(r.type)) res_type = "double";
        else res_type = "int";
        char t[64]; new_temp(g, res_type, t);
        emit(g, op, t, l.place, r.place, node->line);
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "%s", t);
        snprintf(p.type, sizeof(p.type), "%s", res_type);
        return p;
    }

    if (strcmp(node->node_type, "NODE_UNARY_OP") == 0) {
        if (strcmp(node->op, "-") == 0) {
            Place v = gen_expr(g, node->children[0]);
            char t[64]; new_temp(g, v.type, t);
            emit(g, "neg", t, v.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p));
            snprintf(p.place, sizeof(p.place), "%s", t);
            snprintf(p.type, sizeof(p.type), "%s", v.type);
            return p;
        }
        if (strcmp(node->op, "!") == 0) {
            Place v = gen_expr(g, node->children[0]);
            char t[64]; new_temp(g, "int", t);
            emit(g, "!", t, v.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p));
            snprintf(p.place, sizeof(p.place), "%s", t); strcpy(p.type, "int");
            return p;
        }
        if (strcmp(node->op, "&") == 0) {
            ASTNode* inner = node->children[0];
            Place p; memset(&p, 0, sizeof(p));
            if (strcmp(inner->node_type, "NODE_IDENTIFIER") == 0) {
                char t[64]; new_temp(g, "ptr", t);
                emit(g, "ADDR", t, inner->identifier, "0", inner->line);
                snprintf(p.place, sizeof(p.place), "%s", t);
            } else if (strcmp(inner->node_type, "NODE_INDEX") == 0) {
                char addr[64];
                gen_index_addr(g, inner, addr);
                snprintf(p.place, sizeof(p.place), "%s", addr);
            } else {
                char t[64]; new_temp(g, "ptr", t);
                emit(g, "ADDR", t, "0", "0", node->line);
                snprintf(p.place, sizeof(p.place), "%s", t);
            }
            strcpy(p.type, "ptr");
            return p;
        }
        if (strcmp(node->op, "*") == 0) {
            Place pv = gen_expr(g, node->children[0]);
            /* deref type */
            char dtype[64];
            strcpy(dtype, "int");
            if (strcmp(node->children[0]->node_type, "NODE_IDENTIFIER") == 0) {
                SymRec* rec = find_any_symbol(g, node->children[0]->identifier);
                if (rec) {
                    size_t n = strlen(rec->type);
                    if (n > 0 && rec->type[n - 1] == '*') {
                        snprintf(dtype, sizeof(dtype), "%.*s", (int)(n - 1), rec->type);
                        dtype[n - 1] = '\0';
                    }
                }
            }
            char t[64]; new_temp(g, dtype, t);
            emit(g, "LOAD_PTR", t, pv.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p));
            snprintf(p.place, sizeof(p.place), "%s", t);
            const char* tt = temp_type_of(&g->temp_types, t);
            snprintf(p.type, sizeof(p.type), "%s", tt ? tt : "int");
            return p;
        }
        if (strcmp(node->op, "++") == 0 || strcmp(node->op, "--") == 0 ||
            strcmp(node->op, "p++") == 0 || strcmp(node->op, "p--") == 0) {
            Place p;
            gen_inc_dec(g, node, &p);
            return p;
        }
        return gen_expr(g, node->children[0]);
    }

    if (strcmp(node->node_type, "NODE_INDEX") == 0) {
        char addr[64];
        gen_index_addr(g, node, addr);
        char t[64]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, addr, "", node->line);
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "%s", t); strcpy(p.type, "int");
        return p;
    }

    if (strcmp(node->node_type, "NODE_MEMBER") == 0) {
        char addr[64], ftype[64];
        if (!gen_member_addr(g, node, addr, ftype)) return zero;
        const char* tt = (strcmp(ftype, "double") == 0 || strcmp(ftype, "float") == 0) ? "double" : "int";
        char t[64]; new_temp(g, tt, t);
        emit(g, "LOAD_PTR", t, addr, "", node->line);
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "%s", t);
        const char* ty = temp_type_of(&g->temp_types, t);
        snprintf(p.type, sizeof(p.type), "%s", ty ? ty : "int");
        return p;
    }

    if (strcmp(node->node_type, "NODE_FUNC_CALL") == 0) {
        Place p;
        gen_call(g, node, &p);
        return p;
    }

    if (strcmp(node->node_type, "NODE_ASSIGNMENT") == 0 || strcmp(node->node_type, "NODE_COMPOUND_ASSIGN") == 0) {
        Place p;
        gen_assignment(g, node, &p);
        return p;
    }

    return zero;
}

static void gen_var_decl(Gen* g, ASTNode* node) {
    if (node->is_array) {
        int start = node->has_size ? 1 : 0;
        int idx = 0;
        for (int i = start; i < node->child_count; i++, idx++) {
            Place v = gen_expr(g, node->children[i]);
            char idx_t[64]; new_temp(g, "ptr", idx_t);
            char istr[16]; snprintf(istr, sizeof(istr), "%d", idx);
            emit(g, "IDX_ADDR", idx_t, node->identifier, istr, node->line);
            emit(g, "STORE_PTR", "", idx_t, v.place, node->line);
        }
        return;
    }
    if (node->child_count > 0) {
        Place v = gen_expr(g, node->children[0]);
        emit(g, "=", node->identifier, v.place, "", node->line);
    }
}

static void gen_stmt(Gen* g, ASTNode* node) {
    if (!node) return;
    const char* nt = node->node_type;
    if (strcmp(nt, "NODE_EMPTY") == 0 || strcmp(nt, "NODE_ERROR") == 0) return;

    if (strcmp(nt, "NODE_DECL_LIST") == 0) {
        for (int i = 0; i < node->child_count; i++) gen_stmt(g, node->children[i]);
        return;
    }
    if (strcmp(nt, "NODE_VAR_DECL") == 0) { gen_var_decl(g, node); return; }
    if (strcmp(nt, "NODE_COMPOUND_STMT") == 0) {
        for (int i = 0; i < node->child_count; i++) gen_stmt(g, node->children[i]);
        return;
    }
    if (strcmp(nt, "NODE_EXPRESSION_STMT") == 0) {
        for (int i = 0; i < node->child_count; i++) gen_expr(g, node->children[i]);
        return;
    }
    if (strcmp(nt, "NODE_IF_STMT") == 0) {
        Place cond = gen_expr(g, node->children[0]);
        char lelse[16], lend[16];
        new_label(g, lelse); new_label(g, lend);
        emit(g, "IF_FALSE", lelse, cond.place, "", node->line);
        gen_stmt(g, node->children[1]);
        if (node->child_count > 2) {
            emit(g, "GOTO", lend, "", "", node->line);
            emit(g, "LABEL", lelse, "", "", node->line);
            gen_stmt(g, node->children[2]);
            emit(g, "LABEL", lend, "", "", node->line);
        } else {
            emit(g, "LABEL", lelse, "", "", node->line);
        }
        return;
    }
    if (strcmp(nt, "NODE_WHILE_STMT") == 0) {
        char lstart[16], lend[16];
        new_label(g, lstart); new_label(g, lend);
        if (g->loop_depth < 32) {
            strcpy(g->loop_brk[g->loop_depth], lend);
            strcpy(g->loop_cont[g->loop_depth], lstart);
            g->loop_depth++;
        }
        emit(g, "LABEL", lstart, "", "", node->line);
        Place cond = gen_expr(g, node->children[0]);
        emit(g, "IF_FALSE", lend, cond.place, "", node->line);
        gen_stmt(g, node->children[1]);
        emit(g, "GOTO", lstart, "", "", node->line);
        emit(g, "LABEL", lend, "", "", node->line);
        g->loop_depth--;
        return;
    }
    if (strcmp(nt, "NODE_FOR_STMT") == 0) {
        ASTNode* init = node->children[0];
        ASTNode* cond = node->children[1];
        ASTNode* incr = node->children[2];
        ASTNode* body = node->children[3];
        char lstart[16], lstep[16], lend[16];
        new_label(g, lstart); new_label(g, lstep); new_label(g, lend);
        gen_stmt(g, init);
        if (g->loop_depth < 32) {
            strcpy(g->loop_brk[g->loop_depth], lend);
            strcpy(g->loop_cont[g->loop_depth], lstep);
            g->loop_depth++;
        }
        emit(g, "LABEL", lstart, "", "", node->line);
        if (strcmp(cond->node_type, "NODE_EMPTY") != 0) {
            Place c = gen_expr(g, cond);
            emit(g, "IF_FALSE", lend, c.place, "", node->line);
        }
        gen_stmt(g, body);
        emit(g, "LABEL", lstep, "", "", node->line);
        if (strcmp(incr->node_type, "NODE_EMPTY") != 0) gen_expr(g, incr);
        emit(g, "GOTO", lstart, "", "", node->line);
        emit(g, "LABEL", lend, "", "", node->line);
        g->loop_depth--;
        return;
    }
    if (strcmp(nt, "NODE_BREAK_STMT") == 0) {
        if (g->loop_depth > 0) emit(g, "GOTO", g->loop_brk[g->loop_depth - 1], "", "", node->line);
        return;
    }
    if (strcmp(nt, "NODE_CONTINUE_STMT") == 0) {
        if (g->loop_depth > 0) emit(g, "GOTO", g->loop_cont[g->loop_depth - 1], "", "", node->line);
        return;
    }
    if (strcmp(nt, "NODE_RETURN_STMT") == 0) {
        if (node->child_count > 0) {
            Place v = gen_expr(g, node->children[0]);
            emit(g, "RETURN", "", v.place, "", node->line);
        } else {
            emit(g, "RETURN", "", "", "", node->line);
        }
        return;
    }
}

static void emit_global_inits(Gen* g, ASTNode* ast) {
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* top = ast->children[i];
        ASTNode** decls;
        int decl_count;
        if (strcmp(top->node_type, "NODE_DECL_LIST") == 0) { decls = top->children; decl_count = top->child_count; }
        else { decls = &top; decl_count = 1; }
        for (int d = 0; d < decl_count; d++) {
            ASTNode* node = decls[d];
            if (strcmp(node->node_type, "NODE_VAR_DECL") != 0) continue;
            if (node->is_array) gen_var_decl(g, node);
            else if (node->child_count > 0) {
                Place v = gen_expr(g, node->children[0]);
                emit(g, "=", node->identifier, v.place, "", node->line);
            }
        }
    }
}

TACGen* generate_tac(ASTNode* ast, SemModel* sem, DiagList* diags, StrPool* pool) {
    TACGen* out = (TACGen*)xcalloc(1, sizeof(TACGen));
    Gen g;
    memset(&g, 0, sizeof(Gen));
    g.sem = sem;
    g.diags = diags;
    g.pool = pool;
    g.instrs = taclist_new();
    templist_init(&g.temp_types);
    strlist_init(&g.strings);

    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* top = ast->children[i];
        if (strcmp(top->node_type, "NODE_FUNCTION_DEF") != 0) continue;
        emit(&g, "FUNC_BEGIN", top->identifier, "", "", top->line);
        if (strcmp(top->identifier, "main") == 0) emit_global_inits(&g, ast);
        ASTNode* body = NULL;
        for (int c = 0; c < top->child_count; c++) {
            if (strcmp(top->children[c]->node_type, "NODE_PARAMETER") != 0) { body = top->children[c]; break; }
        }
        gen_stmt(&g, body);
        /* implicit return when control reaches the end of the body */
        TACInstr* last = g.instrs->count > 0 ? &g.instrs->items[g.instrs->count - 1] : NULL;
        if (!last || strcmp(last->op, "RETURN") != 0) {
            emit(&g, "RETURN", "", strcmp(top->identifier, "main") == 0 ? "0" : "", "", top->line);
        }
        emit(&g, "FUNC_END", top->identifier, "", "", top->line);
    }

    out->instrs = g.instrs;
    out->temp_types = g.temp_types;
    out->strings = g.strings;
    out->temp_count = g.temp_count;
    out->label_count = g.label_count;
    return out;
}

void tac_gen_free(TACGen* g) {
    if (!g) return;
    tac_list_free(g->instrs);
    free(g->temp_types.items);
    free(g->strings.items); /* strings owned by the pool */
    free(g);
}
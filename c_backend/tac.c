#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }
static char *xstrdup(const char *s) { char *d = strdup(s); if (!d) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return d; }

static long long truncate_ll(double v) {
    const double LIM = 9007199254740991.0;
    if (v >= LIM) return 9007199254740991LL;
    if (v <= -LIM) return -9007199254740991LL;
    return (long long)v;
}

typedef struct {
    TacResult *r;
    SemResult *sem;
    DiagList *diags;
    /* per-function goto label registry */
    char (*labelNames)[128];
    char (*labelIds)[32];
    int labelReg_count, labelReg_capacity;
} G;

static void emit(G *g, const char *op, const char *res, const char *a1, const char *a2, int line) {
    TacResult *r = g->r;
    if (r->count >= r->capacity) { r->capacity = r->capacity ? r->capacity * 2 : 64; r->items = (TacInstr *)xrealloc(r->items, sizeof(TacInstr) * (size_t)r->capacity); }
    TacInstr *ins = &r->items[r->count++];
    memset(ins, 0, sizeof(TacInstr));
    strncpy(ins->op, op, sizeof(ins->op) - 1);
    if (res) strncpy(ins->res, res, sizeof(ins->res) - 1);
    if (a1) strncpy(ins->a1, a1, sizeof(ins->a1) - 1);
    if (a2) strncpy(ins->a2, a2, sizeof(ins->a2) - 1);
    ins->line = line;
}

static void new_temp(G *g, const char *type, char out[32]) {
    snprintf(out, 32, "t%d", g->r->tempCount++);
    if (type) {
        TacResult *r = g->r;
        if (r->tempType_count >= r->tempType_capacity) {
            r->tempType_capacity = r->tempType_capacity ? r->tempType_capacity * 2 : 64;
            r->tempNames = (char (*)[32])xrealloc(r->tempNames, sizeof(char[32]) * (size_t)r->tempType_capacity);
            r->tempTypes = (char (*)[16])xrealloc(r->tempTypes, sizeof(char[16]) * (size_t)r->tempType_capacity);
        }
        strncpy(r->tempNames[r->tempType_count], out, 31);
        strncpy(r->tempTypes[r->tempType_count], type, 15);
        r->tempType_count++;
    }
}

static void new_label(G *g, char out[32]) {
    snprintf(out, 32, "L%d", g->r->labelCount++);
}

static int intern_string(G *g, const char *s) {
    TacResult *r = g->r;
    if (r->string_count >= r->string_capacity) {
        r->string_capacity = r->string_capacity ? r->string_capacity * 2 : 32;
        r->strings = (char **)xrealloc(r->strings, sizeof(char *) * (size_t)r->string_capacity);
    }
    r->strings[r->string_count] = xstrdup(s ? s : "");
    return r->string_count++;
}

typedef struct { char place[64]; char type[16]; int isConst; } Place;

static void const_string_v(double v, const char *type, char out[64]) {
    int isF = (strcmp(type, "float") == 0 || strcmp(type, "double") == 0);
    if (isF) {
        nova_fmt_float_const(v, out, 64);
    } else {
        snprintf(out, 64, "%lld", truncate_ll(v));
    }
}

static int is_float_ty(const char *t) { return strcmp(t, "float") == 0 || strcmp(t, "double") == 0; }

/* find a symbol anywhere (globals then frames) */
static SymRec *find_any_symbol(G *g, const char *name) {
    SymRec *r = sem_find_global(g->sem, name);
    if (r) return r;
    for (int i = 0; i < g->sem->func_count; i++) {
        r = sem_find_in_frame(&g->sem->functions[i], name);
        if (r) return r;
    }
    return NULL;
}

static const char *sem_expr_type_ident(G *g, const char *name) {
    SymRec *r = find_any_symbol(g, name);
    return r ? r->type : "int";
}

/* forward decls */
static Place gen_expr(G *g, NovaNode *node);
static void gen_stmt(G *g, NovaNode *node);

static void gen_index_addr(G *g, NovaNode *node, char out[32]) {
    NovaNode *base = node->children[0];
    Place idx = gen_expr(g, node->children[1]);
    char t[32];
    new_temp(g, "ptr", t);
    char baseName[64];
    if (base->type == NODE_IDENTIFIER) strncpy(baseName, base->identifier, 63);
    else strcpy(baseName, "0");
    baseName[63] = '\0';
    emit(g, "IDX_ADDR", t, baseName, idx.place, node->line);
    strcpy(out, t);
}

typedef struct { char place[32]; char fieldType[160]; int ok; } MemberAddr;

static MemberAddr gen_member_addr(G *g, NovaNode *node) {
    MemberAddr ma; memset(&ma, 0, sizeof(ma));
    NovaNode *base = node->children[0];
    if (base && base->type == NODE_IDENTIFIER) {
        SymRec *rec = find_any_symbol(g, base->identifier);
        if (rec && strncmp(rec->type, "struct ", 7) == 0) {
            StructDef *st = sem_find_struct(g->sem, rec->type + 7);
            if (st) {
                for (int i = 0; i < st->field_count; i++) {
                    if (strcmp(st->fields[i].name, node->identifier) == 0) {
                        char t[32];
                        new_temp(g, "ptr", t);
                        char off[32];
                        snprintf(off, sizeof(off), "%d", st->fields[i].offset);
                        emit(g, "ADDR", t, base->identifier, off, node->line);
                        strcpy(ma.place, t);
                        if (st->fields[i].is_array) snprintf(ma.fieldType, sizeof(ma.fieldType), "%s*", st->fields[i].type);
                        else strncpy(ma.fieldType, st->fields[i].type, sizeof(ma.fieldType) - 1);
                        ma.ok = 1;
                        return ma;
                    }
                }
            }
        }
    }
    diag_add(g->diags, "error", node->line, 1, "Invalid member access");
    return ma;
}

/* lvalue addressing */
typedef struct { int valid; int direct; char name[64]; char place[32]; } LValue;

static LValue lvalue_addr(G *g, NovaNode *node);

static void store_to_lvalue(G *g, LValue *lv, const char *valuePlace, int line) {
    if (!lv->valid) return;
    if (lv->direct) emit(g, "=", lv->name, valuePlace, "", line);
    else emit(g, "STORE_PTR", "", lv->place, valuePlace, line);
}

static void gen_assignment(G *g, NovaNode *node, Place *out) {
    NovaNode *target = node->children[0];
    if (node->type == NODE_ASSIGNMENT) {
        Place rhs = gen_expr(g, node->children[1]);
        LValue lv = lvalue_addr(g, target);
        store_to_lvalue(g, &lv, rhs.place, node->line);
        *out = rhs;
        return;
    }
    const char *op = NULL;
    if (strcmp(node->op, "+=") == 0) op = "+";
    else if (strcmp(node->op, "-=") == 0) op = "-";
    else if (strcmp(node->op, "*=") == 0) op = "*";
    else if (strcmp(node->op, "/=") == 0) op = "/";
    else if (strcmp(node->op, "%=") == 0) op = "%";
    else if (strcmp(node->op, "&=") == 0) op = "&";
    else if (strcmp(node->op, "|=") == 0) op = "|";
    else if (strcmp(node->op, "^=") == 0) op = "^";
    else if (strcmp(node->op, "<<=") == 0) op = "<<";
    else if (strcmp(node->op, ">>=") == 0) op = ">>";
    if (!op) { memset(out, 0, sizeof(Place)); strcpy(out->type, "int"); return; }
    LValue lv = lvalue_addr(g, target);
    if (!lv.valid) { memset(out, 0, sizeof(Place)); strcpy(out->type, "int"); return; }
    Place oldV; memset(&oldV, 0, sizeof(oldV));
    if (lv.direct) {
        strncpy(oldV.place, lv.name, sizeof(oldV.place) - 1);
        strncpy(oldV.type, sem_expr_type_ident(g, target->identifier), sizeof(oldV.type) - 1);
    } else {
        char t[32]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, lv.place, "", node->line);
        strcpy(oldV.place, t);
        strcpy(oldV.type, "int");
    }
    Place rhs = gen_expr(g, node->children[1]);
    const char *resType = (is_float_ty(oldV.type) || is_float_ty(rhs.type)) ? "double" : "int";
    char t[32]; new_temp(g, resType, t);
    emit(g, op, t, oldV.place, rhs.place, node->line);
    store_to_lvalue(g, &lv, t, node->line);
    memset(out, 0, sizeof(Place));
    strcpy(out->place, t);
    strcpy(out->type, resType);
}

static void gen_incdec(G *g, NovaNode *node, Place *out) {
    const char *op = (strcmp(node->op, "++") == 0 || strcmp(node->op, "p++") == 0) ? "+" : "-";
    int prefix = (strcmp(node->op, "++") == 0 || strcmp(node->op, "--") == 0);
    NovaNode *target = node->children[0];
    LValue lv = lvalue_addr(g, target);
    if (!lv.valid) { memset(out, 0, sizeof(Place)); strcpy(out->type, "int"); out->isConst = 1; return; }
    Place oldV; memset(&oldV, 0, sizeof(oldV));
    if (lv.direct) {
        strncpy(oldV.place, lv.name, sizeof(oldV.place) - 1);
        strncpy(oldV.type, sem_expr_type_ident(g, target->identifier), sizeof(oldV.type) - 1);
    } else {
        char t[32]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, lv.place, "", node->line);
        strcpy(oldV.place, t);
        strcpy(oldV.type, "int");
    }
    char t[32]; new_temp(g, strcmp(oldV.type, "double") == 0 ? "double" : "int", t);
    emit(g, op, t, oldV.place, "1", node->line);
    store_to_lvalue(g, &lv, t, node->line);
    if (prefix) {
        memset(out, 0, sizeof(Place));
        strcpy(out->place, t);
        strcpy(out->type, strcmp(oldV.type, "double") == 0 ? "double" : "int");
    } else {
        *out = oldV;
    }
}

static LValue lvalue_addr(G *g, NovaNode *node) {
    LValue lv; memset(&lv, 0, sizeof(lv));
    if (node->type == NODE_IDENTIFIER) {
        lv.valid = 1; lv.direct = 1;
        strncpy(lv.name, node->identifier, sizeof(lv.name) - 1);
        return lv;
    }
    if (node->type == NODE_INDEX) {
        lv.valid = 1; lv.direct = 0;
        gen_index_addr(g, node, lv.place);
        return lv;
    }
    if (node->type == NODE_MEMBER) {
        MemberAddr ma = gen_member_addr(g, node);
        if (ma.ok) { lv.valid = 1; lv.direct = 0; strcpy(lv.place, ma.place); }
        return lv;
    }
    if (node->type == NODE_UNARY_OP && strcmp(node->op, "*") == 0) {
        Place p = gen_expr(g, node->children[0]);
        lv.valid = 1; lv.direct = 0;
        strncpy(lv.place, p.place, sizeof(lv.place) - 1);
        return lv;
    }
    return lv;
}

static void gen_call(G *g, NovaNode *node, Place *out) {
    int isBuiltinIO = (strcmp(node->identifier, "printf") == 0 || strcmp(node->identifier, "scanf") == 0);
    if (isBuiltinIO) {
        NovaNode *first = node->child_count > 0 ? node->children[0] : NULL;
        const char *fmt = "";
        if (first && first->type == NODE_STRING_LITERAL) fmt = first->string_val;
        else if (first) diag_add(g->diags, "error", node->line, 1, "%s format must be a string literal", node->identifier);
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
    char t[32]; new_temp(g, "int", t);
    char nargs[16];
    snprintf(nargs, sizeof(nargs), "%d", node->child_count);
    emit(g, "CALL", t, node->identifier, nargs, node->line);
    memset(out, 0, sizeof(Place));
    strcpy(out->place, t);
    strcpy(out->type, "int");
}

static Place gen_expr(G *g, NovaNode *node) {
    Place zero; memset(&zero, 0, sizeof(zero));
    strcpy(zero.place, "0"); strcpy(zero.type, "int"); zero.isConst = 1;
    if (!node || node->type == NODE_ERROR) return zero;

    if (node->type == NODE_INT_LITERAL) {
        Place p; memset(&p, 0, sizeof(p));
        snprintf(p.place, sizeof(p.place), "%lld", truncate_ll(node->num_val));
        strcpy(p.type, "int"); p.isConst = 1;
        return p;
    }
    if (node->type == NODE_FLOAT_LITERAL) {
        Place p; memset(&p, 0, sizeof(p));
        const_string_v(node->num_val, "double", p.place);
        strcpy(p.type, "double"); p.isConst = 1;
        return p;
    }
    if (node->type == NODE_STRING_LITERAL) {
        Place p; memset(&p, 0, sizeof(p));
        int idx = intern_string(g, node->string_val);
        snprintf(p.place, sizeof(p.place), "\"str%d\"", idx);
        strcpy(p.type, "char*"); p.isConst = 1;
        return p;
    }
    if (node->type == NODE_IDENTIFIER) {
        Place p; memset(&p, 0, sizeof(p));
        strncpy(p.place, node->identifier, sizeof(p.place) - 1);
        strncpy(p.type, sem_expr_type_ident(g, node->identifier), sizeof(p.type) - 1);
        return p;
    }
    if (node->type == NODE_BINARY_OP) {
        const char *op = node->op;
        if (strcmp(op, "&&") == 0) {
            Place a = gen_expr(g, node->children[0]);
            char t[32]; new_temp(g, "int", t);
            char lShort[32]; new_label(g, lShort);
            emit(g, "=", t, a.place, "", node->line);
            emit(g, "IF_FALSE", lShort, t, "", node->line);
            Place bv = gen_expr(g, node->children[1]);
            emit(g, "=", t, bv.place, "", node->line);
            emit(g, "LABEL", lShort, "", "", node->line);
            emit(g, "!=", t, t, "0", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "int");
            return p;
        }
        if (strcmp(op, "||") == 0) {
            Place a = gen_expr(g, node->children[0]);
            char t[32]; new_temp(g, "int", t);
            char lCont[32], lNorm[32]; new_label(g, lCont); new_label(g, lNorm);
            emit(g, "=", t, a.place, "", node->line);
            emit(g, "IF_FALSE", lCont, t, "", node->line);
            emit(g, "GOTO", lNorm, "", "", node->line);
            emit(g, "LABEL", lCont, "", "", node->line);
            Place bv = gen_expr(g, node->children[1]);
            emit(g, "=", t, bv.place, "", node->line);
            emit(g, "LABEL", lNorm, "", "", node->line);
            emit(g, "!=", t, t, "0", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "int");
            return p;
        }
        Place l = gen_expr(g, node->children[0]);
        Place r = gen_expr(g, node->children[1]);
        int isBitwise = (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
                         strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0);
        int isCmp = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
                     strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0);
        const char *resType = (isCmp || isBitwise) ? "int"
            : ((is_float_ty(l.type) || is_float_ty(r.type)) ? "double" : "int");
        char t[32]; new_temp(g, resType, t);
        emit(g, op, t, l.place, r.place, node->line);
        Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, resType);
        return p;
    }
    if (node->type == NODE_TERNARY) {
        Place cond = gen_expr(g, node->children[0]);
        char t[32]; new_temp(g, "int", t);
        char lf[32], le[32]; new_label(g, lf); new_label(g, le);
        emit(g, "IF_FALSE", lf, cond.place, "", node->line);
        Place tv = gen_expr(g, node->children[1]);
        emit(g, "=", t, tv.place, "", node->line);
        emit(g, "GOTO", le, "", "", node->line);
        emit(g, "LABEL", lf, "", "", node->line);
        Place ev = gen_expr(g, node->children[2]);
        emit(g, "=", t, ev.place, "", node->line);
        emit(g, "LABEL", le, "", "", node->line);
        const char *resType = (strcmp(tv.type, "double") == 0 || strcmp(ev.type, "double") == 0) ? "double" : "int";
        for (int i = 0; i < g->r->tempType_count; i++) {
            if (strcmp(g->r->tempNames[i], t) == 0) { strncpy(g->r->tempTypes[i], resType, 15); break; }
        }
        Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, resType);
        return p;
    }
    if (node->type == NODE_CAST) {
        Place v = gen_expr(g, node->children[0]);
        const char *tn = node->has_type_name ? node->type_name : "int";
        size_t tnlen = strlen(tn);
        if ((tnlen > 0 && tn[tnlen - 1] == '*') || strcmp(tn, "void") == 0) return v;
        int isF = (strcmp(tn, "double") == 0 || strcmp(tn, "float") == 0);
        char t[32]; new_temp(g, isF ? "double" : "int", t);
        emit(g, isF ? "CAST_F" : "CAST_I", t, v.place, "", node->line);
        Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, isF ? "double" : "int");
        return p;
    }
    if (node->type == NODE_SIZEOF) {
        NovaNode *child = node->children[0];
        if (child && child->type == NODE_IDENTIFIER) {
            SymRec *rec = find_any_symbol(g, child->identifier);
            if (rec) {
                Place p; memset(&p, 0, sizeof(p));
                if (rec->is_array) snprintf(p.place, sizeof(p.place), "%d", rec->size * 4);
                else if (rec->type[0] && rec->type[strlen(rec->type) - 1] == '*') strcpy(p.place, "8");
                else if (strcmp(rec->type, "double") == 0) strcpy(p.place, "8");
                else strcpy(p.place, "4");
                strcpy(p.type, "int"); p.isConst = 1;
                return p;
            }
        }
        Place v = gen_expr(g, child);
        Place p; memset(&p, 0, sizeof(p));
        int big = (strcmp(v.type, "double") == 0 || strcmp(v.type, "ptr") == 0 ||
                   (v.type[0] && v.type[strlen(v.type) - 1] == '*'));
        strcpy(p.place, big ? "8" : "4");
        strcpy(p.type, "int"); p.isConst = 1;
        return p;
    }
    if (node->type == NODE_UNARY_OP) {
        if (strcmp(node->op, "-") == 0) {
            Place v = gen_expr(g, node->children[0]);
            char t[32]; new_temp(g, v.type, t);
            emit(g, "neg", t, v.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, v.type);
            return p;
        }
        if (strcmp(node->op, "!") == 0) {
            Place v = gen_expr(g, node->children[0]);
            char t[32]; new_temp(g, "int", t);
            emit(g, "!", t, v.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "int");
            return p;
        }
        if (strcmp(node->op, "~") == 0) {
            Place v = gen_expr(g, node->children[0]);
            char t[32]; new_temp(g, "int", t);
            emit(g, "~", t, v.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "int");
            return p;
        }
        if (strcmp(node->op, "&") == 0) {
            NovaNode *inner = node->children[0];
            if (inner->type == NODE_IDENTIFIER) {
                char t[32]; new_temp(g, "ptr", t);
                emit(g, "ADDR", t, inner->identifier, "0", inner->line);
                Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "ptr");
                return p;
            }
            if (inner->type == NODE_INDEX) {
                char addr[32];
                gen_index_addr(g, inner, addr);
                Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, addr); strcpy(p.type, "ptr");
                return p;
            }
            char t[32]; new_temp(g, "ptr", t);
            emit(g, "ADDR", t, "0", "0", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "ptr");
            return p;
        }
        if (strcmp(node->op, "*") == 0) {
            Place pv = gen_expr(g, node->children[0]);
            char dtype[16] = "int";
            if (node->children[0]->type == NODE_IDENTIFIER) {
                SymRec *rec = find_any_symbol(g, node->children[0]->identifier);
                if (rec && rec->type[0] && rec->type[strlen(rec->type) - 1] == '*') {
                    strncpy(dtype, rec->type, strlen(rec->type) - 1);
                    dtype[strlen(rec->type) - 1] = '\0';
                }
            }
            char t[32]; new_temp(g, dtype, t);
            emit(g, "LOAD_PTR", t, pv.place, "", node->line);
            Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, dtype);
            return p;
        }
        if (strcmp(node->op, "++") == 0 || strcmp(node->op, "--") == 0 ||
            strcmp(node->op, "p++") == 0 || strcmp(node->op, "p--") == 0) {
            Place p; gen_incdec(g, node, &p);
            return p;
        }
        return gen_expr(g, node->children[0]);
    }
    if (node->type == NODE_INDEX) {
        char addr[32];
        gen_index_addr(g, node, addr);
        char t[32]; new_temp(g, "int", t);
        emit(g, "LOAD_PTR", t, addr, "", node->line);
        Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, "int");
        return p;
    }
    if (node->type == NODE_MEMBER) {
        MemberAddr ma = gen_member_addr(g, node);
        if (!ma.ok) return zero;
        int isF = (strcmp(ma.fieldType, "double") == 0 || strcmp(ma.fieldType, "float") == 0);
        char t[32]; new_temp(g, isF ? "double" : "int", t);
        emit(g, "LOAD_PTR", t, ma.place, "", node->line);
        Place p; memset(&p, 0, sizeof(p)); strcpy(p.place, t); strcpy(p.type, isF ? "double" : "int");
        return p;
    }
    if (node->type == NODE_FUNC_CALL) {
        Place p; gen_call(g, node, &p);
        return p;
    }
    if (node->type == NODE_ASSIGNMENT || node->type == NODE_COMPOUND_ASSIGN) {
        Place p; gen_assignment(g, node, &p);
        return p;
    }
    return zero;
}

/* ------------------------------ statements ------------------------------- */

typedef struct { char brk[32]; char cont[32]; int isSwitch; } LoopCtx;

static LoopCtx g_loopStack[64];
static int g_loopDepth = 0;

static const char *find_label(G *g, const char *name) {
    for (int i = 0; i < g->labelReg_count; i++) {
        if (strcmp(g->labelNames[i], name) == 0) return g->labelIds[i];
    }
    return NULL;
}
static const char *register_label(G *g, const char *name) {
    const char *existing = find_label(g, name);
    if (existing) return existing;
    if (g->labelReg_count >= g->labelReg_capacity) {
        g->labelReg_capacity = g->labelReg_capacity ? g->labelReg_capacity * 2 : 16;
        g->labelNames = (char (*)[128])xrealloc(g->labelNames, sizeof(char[128]) * (size_t)g->labelReg_capacity);
        g->labelIds = (char (*)[32])xrealloc(g->labelIds, sizeof(char[32]) * (size_t)g->labelReg_capacity);
    }
    strncpy(g->labelNames[g->labelReg_count], name, 127);
    new_label(g, g->labelIds[g->labelReg_count]);
    g->labelReg_count++;
    return g->labelIds[g->labelReg_count - 1];
}
static void collect_labels(G *g, NovaNode *node) {
    if (!node) return;
    if (node->type == NODE_LABEL_STMT) register_label(g, node->identifier);
    for (int i = 0; i < node->child_count; i++) collect_labels(g, node->children[i]);
}

static void emit_var_decl_init(G *g, NovaNode *node) {
    if (node->is_array) {
        int start = node->has_size ? 1 : 0;
        int idx = 0;
        for (int i = start; i < node->child_count; i++, idx++) {
            Place v = gen_expr(g, node->children[i]);
            char idxT[32]; new_temp(g, "ptr", idxT);
            char istr[16]; snprintf(istr, sizeof(istr), "%d", idx);
            emit(g, "IDX_ADDR", idxT, node->identifier, istr, node->line);
            emit(g, "STORE_PTR", "", idxT, v.place, node->line);
        }
        return;
    }
    if (node->child_count > 0) {
        Place v = gen_expr(g, node->children[0]);
        emit(g, "=", node->identifier, v.place, "", node->line);
    }
}

static void gen_var_decl(G *g, NovaNode *node) {
    if (node->is_static) return; /* initialized once in global-init prologue */
    emit_var_decl_init(g, node);
}

static void gen_stmt(G *g, NovaNode *node) {
    if (!node || node->type == NODE_EMPTY || node->type == NODE_ERROR) return;
    if (node->type == NODE_DECL_LIST) { for (int i = 0; i < node->child_count; i++) gen_stmt(g, node->children[i]); return; }
    if (node->type == NODE_VAR_DECL) { gen_var_decl(g, node); return; }
    if (node->type == NODE_COMPOUND_STMT) { for (int i = 0; i < node->child_count; i++) gen_stmt(g, node->children[i]); return; }
    if (node->type == NODE_EXPRESSION_STMT) { for (int i = 0; i < node->child_count; i++) gen_expr(g, node->children[i]); return; }

    if (node->type == NODE_IF_STMT) {
        Place cond = gen_expr(g, node->children[0]);
        char lElse[32], lEnd[32]; new_label(g, lElse); new_label(g, lEnd);
        emit(g, "IF_FALSE", lElse, cond.place, "", node->line);
        gen_stmt(g, node->children[1]);
        if (node->child_count > 2) {
            emit(g, "GOTO", lEnd, "", "", node->line);
            emit(g, "LABEL", lElse, "", "", node->line);
            gen_stmt(g, node->children[2]);
            emit(g, "LABEL", lEnd, "", "", node->line);
        } else {
            emit(g, "LABEL", lElse, "", "", node->line);
        }
        return;
    }
    if (node->type == NODE_WHILE_STMT) {
        char lStart[32], lEnd[32]; new_label(g, lStart); new_label(g, lEnd);
        g_loopStack[g_loopDepth].isSwitch = 0;
        strcpy(g_loopStack[g_loopDepth].brk, lEnd);
        strcpy(g_loopStack[g_loopDepth].cont, lStart);
        g_loopDepth++;
        emit(g, "LABEL", lStart, "", "", node->line);
        Place cond = gen_expr(g, node->children[0]);
        emit(g, "IF_FALSE", lEnd, cond.place, "", node->line);
        gen_stmt(g, node->children[1]);
        emit(g, "GOTO", lStart, "", "", node->line);
        emit(g, "LABEL", lEnd, "", "", node->line);
        g_loopDepth--;
        return;
    }
    if (node->type == NODE_DO_WHILE_STMT) {
        char lStart[32], lEnd[32]; new_label(g, lStart); new_label(g, lEnd);
        g_loopStack[g_loopDepth].isSwitch = 0;
        strcpy(g_loopStack[g_loopDepth].brk, lEnd);
        strcpy(g_loopStack[g_loopDepth].cont, lStart);
        g_loopDepth++;
        emit(g, "LABEL", lStart, "", "", node->line);
        gen_stmt(g, node->children[0]);
        Place cond = gen_expr(g, node->children[1]);
        emit(g, "IF_FALSE", lEnd, cond.place, "", node->line);
        emit(g, "GOTO", lStart, "", "", node->line);
        emit(g, "LABEL", lEnd, "", "", node->line);
        g_loopDepth--;
        return;
    }
    if (node->type == NODE_SWITCH_STMT) {
        Place sel = gen_expr(g, node->children[0]);
        char lEnd[32]; new_label(g, lEnd);
        g_loopStack[g_loopDepth].isSwitch = 1;
        strcpy(g_loopStack[g_loopDepth].brk, lEnd);
        g_loopStack[g_loopDepth].cont[0] = '\0';
        g_loopDepth++;
        int ncases = node->child_count - 1;
        char (*caseLabels)[32] = (char (*)[32])xmalloc(sizeof(char[32]) * (ncases > 0 ? ncases : 1));
        char defaultLabel[32] = "";
        for (int i = 1; i < node->child_count; i++) {
            new_label(g, caseLabels[i - 1]);
            if (node->children[i]->type == NODE_DEFAULT) strcpy(defaultLabel, caseLabels[i - 1]);
        }
        for (int i = 1; i < node->child_count; i++) {
            NovaNode *c = node->children[i];
            if (c->type == NODE_CASE) {
                char t[32]; new_temp(g, "int", t);
                char valbuf[32]; snprintf(valbuf, sizeof(valbuf), "%lld", truncate_ll(c->num_val));
                emit(g, "==", t, sel.place, valbuf, c->line);
                char lNext[32]; new_label(g, lNext);
                emit(g, "IF_FALSE", lNext, t, "", c->line);
                emit(g, "GOTO", caseLabels[i - 1], "", "", c->line);
                emit(g, "LABEL", lNext, "", "", c->line);
            }
        }
        emit(g, "GOTO", defaultLabel[0] ? defaultLabel : lEnd, "", "", node->line);
        for (int i = 1; i < node->child_count; i++) {
            NovaNode *c = node->children[i];
            emit(g, "LABEL", caseLabels[i - 1], "", "", c->line);
            for (int j = 0; j < c->child_count; j++) gen_stmt(g, c->children[j]);
        }
        emit(g, "LABEL", lEnd, "", "", node->line);
        free(caseLabels);
        g_loopDepth--;
        return;
    }
    if (node->type == NODE_GOTO) {
        const char *lbl = find_label(g, node->identifier);
        if (lbl) emit(g, "GOTO", lbl, "", "", node->line);
        else diag_add(g->diags, "error", node->line, 1, "Use of undefined label '%s'", node->identifier);
        return;
    }
    if (node->type == NODE_LABEL_STMT) {
        const char *lbl = register_label(g, node->identifier);
        emit(g, "LABEL", lbl, "", "", node->line);
        if (node->child_count > 0) gen_stmt(g, node->children[0]);
        return;
    }
    if (node->type == NODE_FOR_STMT) {
        NovaNode *init = node->children[0];
        NovaNode *cond = node->children[1];
        NovaNode *incr = node->children[2];
        NovaNode *body = node->children[3];
        char lStart[32], lStep[32], lEnd[32];
        new_label(g, lStart); new_label(g, lStep); new_label(g, lEnd);
        gen_stmt(g, init);
        g_loopStack[g_loopDepth].isSwitch = 0;
        strcpy(g_loopStack[g_loopDepth].brk, lEnd);
        strcpy(g_loopStack[g_loopDepth].cont, lStep);
        g_loopDepth++;
        emit(g, "LABEL", lStart, "", "", node->line);
        if (cond->type != NODE_EMPTY) {
            Place c = gen_expr(g, cond);
            emit(g, "IF_FALSE", lEnd, c.place, "", node->line);
        }
        gen_stmt(g, body);
        emit(g, "LABEL", lStep, "", "", node->line);
        if (incr->type != NODE_EMPTY) gen_expr(g, incr);
        emit(g, "GOTO", lStart, "", "", node->line);
        emit(g, "LABEL", lEnd, "", "", node->line);
        g_loopDepth--;
        return;
    }
    if (node->type == NODE_BREAK_STMT) {
        if (g_loopDepth > 0) emit(g, "GOTO", g_loopStack[g_loopDepth - 1].brk, "", "", node->line);
        return;
    }
    if (node->type == NODE_CONTINUE_STMT) {
        for (int i = g_loopDepth - 1; i >= 0; i--) {
            if (!g_loopStack[i].isSwitch && g_loopStack[i].cont[0]) {
                emit(g, "GOTO", g_loopStack[i].cont, "", "", node->line);
                return;
            }
        }
        return;
    }
    if (node->type == NODE_RETURN_STMT) {
        if (node->child_count > 0) {
            Place v = gen_expr(g, node->children[0]);
            emit(g, "RETURN", "", v.place, "", node->line);
        } else {
            emit(g, "RETURN", "", "", "", node->line);
        }
        return;
    }
}

static void emit_global_inits(G *g, NovaNode *ast) {
    for (int i = 0; i < ast->child_count; i++) {
        NovaNode *top = ast->children[i];
        NovaNode **decls; int declCount;
        if (top->type == NODE_DECL_LIST) { decls = top->children; declCount = top->child_count; }
        else { decls = &top; declCount = 1; }
        for (int d = 0; d < declCount; d++) {
            if (!decls[d] || decls[d]->type != NODE_VAR_DECL) continue;
            emit_var_decl_init(g, decls[d]);
        }
    }
    for (int i = 0; i < g->sem->static_count; i++) {
        emit_var_decl_init(g, g->sem->staticDecls[i]);
    }
}

TacResult *nova_gen_tac(NovaNode *ast, SemResult *sem, DiagList *diags) {
    G g; memset(&g, 0, sizeof(G));
    g.r = (TacResult *)xcalloc(1, sizeof(TacResult));
    g.sem = sem;
    g.diags = diags;

    for (int i = 0; i < ast->child_count; i++) {
        NovaNode *top = ast->children[i];
        if (top->type != NODE_FUNCTION_DEF) continue;
        if (top->is_forward) continue;
        /* skip forward-declaration placeholder entries too */
        FuncDef *fd = sem_find_function(sem, top->identifier);
        if (fd && fd->is_forward) continue;
        emit(&g, "FUNC_BEGIN", top->identifier, "", "", top->line);
        g.labelReg_count = 0;
        NovaNode *body = NULL;
        for (int c = 0; c < top->child_count; c++) {
            if (top->children[c]->type != NODE_PARAMETER) { body = top->children[c]; break; }
        }
        collect_labels(&g, body);
        if (strcmp(top->identifier, "main") == 0) emit_global_inits(&g, ast);
        g_loopDepth = 0;
        gen_stmt(&g, body);
        TacInstr *last = g.r->count > 0 ? &g.r->items[g.r->count - 1] : NULL;
        if (!last || strcmp(last->op, "RETURN") != 0) {
            emit(&g, "RETURN", "", strcmp(top->identifier, "main") == 0 ? "0" : "", "", top->line);
        }
        emit(&g, "FUNC_END", top->identifier, "", "", top->line);
    }

    free(g.labelNames);
    free(g.labelIds);
    return g.r;
}

void nova_tac_free(TacResult *t) {
    if (!t) return;
    free(t->items);
    /* strings/temp arrays may be borrowed by the optimized list; the owning
     * list is the one with capacity > 0 */
    if (t->string_capacity > 0) {
        for (int i = 0; i < t->string_count; i++) free(t->strings[i]);
        free(t->strings);
    }
    if (t->tempType_capacity > 0) {
        free(t->tempNames);
        free(t->tempTypes);
    }
    free(t);
}

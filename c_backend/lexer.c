#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------------- */
/* Predefined macros (must match PREDEFINED_MACROS in the JS engine)          */
/* ------------------------------------------------------------------------- */

typedef enum { PREDEF_NONE, PREDEF_INT, PREDEF_FLOAT, PREDEF_STRING } PredefKind;

typedef struct { const char *name; PredefKind kind; long long iv; double fv; const char *sv; } Predef;

static const Predef PREDEFS[] = {
    { "__STDC__", PREDEF_INT, 1, 0, NULL },
    { "__STDC_VERSION__", PREDEF_INT, 199901, 0, NULL },
    { "__STDC_HOSTED__", PREDEF_INT, 1, 0, NULL },
    { "__STDC_NO_ATOMICS__", PREDEF_INT, 1, 0, NULL },
    { "__NOVA__", PREDEF_INT, 1, 0, NULL },
    { "__VERSION__", PREDEF_STRING, 0, 0, "NOVA 2.0" },
    { "__FILE__", PREDEF_STRING, 0, 0, "main.c" },
    { "NULL", PREDEF_INT, 0, 0, NULL },
    { "M_PI", PREDEF_FLOAT, 0, 3.141592653589793, NULL },
    { "M_E", PREDEF_FLOAT, 0, 2.718281828459045, NULL },
};

static const Predef *predef_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(PREDEFS) / sizeof(PREDEFS[0]); i++) {
        if (strcmp(PREDEFS[i].name, name) == 0) return &PREDEFS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* #if expression evaluator (defined(), literals, && || ! comparisons)        */
/* ------------------------------------------------------------------------- */

typedef struct { const char *s; int i; int n; } PExpr;

static void pexpr_skipws(PExpr *p) { while (p->i < p->n && isspace((unsigned char)p->s[p->i])) p->i++; }

static long long pexpr_or(PExpr *p);

static long long pexpr_primary(PExpr *p) {
    pexpr_skipws(p);
    if (p->i >= p->n) return 0;
    char c = p->s[p->i];
    if (c == '(') {
        p->i++;
        long long v = pexpr_or(p);
        pexpr_skipws(p);
        if (p->i < p->n && p->s[p->i] == ')') p->i++;
        return v;
    }
    if (c == '!') { p->i++; return pexpr_primary(p) ? 0 : 1; }
    if (isdigit((unsigned char)c)) {
        int start = p->i;
        if (c == '0' && p->i + 1 < p->n && (p->s[p->i + 1] == 'x' || p->s[p->i + 1] == 'X')) {
            p->i += 2;
            while (p->i < p->n && isxdigit((unsigned char)p->s[p->i])) p->i++;
            char buf[64]; int len = p->i - start; if (len > 63) len = 63;
            memcpy(buf, p->s + start, len); buf[len] = '\0';
            return strtoll(buf, NULL, 16);
        }
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
        char buf[64]; int len = p->i - start; if (len > 63) len = 63;
        memcpy(buf, p->s + start, len); buf[len] = '\0';
        if (p->i < p->n && strchr("lLuU", p->s[p->i])) p->i++;
        return strtoll(buf, NULL, 10);
    }
    if (strncmp(p->s + p->i, "defined", 7) == 0 &&
        (p->i + 7 >= p->n || !isalnum((unsigned char)p->s[p->i + 7]))) {
        p->i += 7;
        pexpr_skipws(p);
        int paren = 0;
        if (p->i < p->n && p->s[p->i] == '(') { paren = 1; p->i++; }
        pexpr_skipws(p);
        int start = p->i;
        while (p->i < p->n && (isalnum((unsigned char)p->s[p->i]) || p->s[p->i] == '_')) p->i++;
        char name[128]; int len = p->i - start; if (len > 127) len = 127;
        memcpy(name, p->s + start, len); name[len] = '\0';
        pexpr_skipws(p);
        if (paren && p->i < p->n && p->s[p->i] == ')') p->i++;
        return predef_lookup(name) ? 1 : 0;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        int start = p->i;
        while (p->i < p->n && (isalnum((unsigned char)p->s[p->i]) || p->s[p->i] == '_')) p->i++;
        char name[128]; int len = p->i - start; if (len > 127) len = 127;
        memcpy(name, p->s + start, len); name[len] = '\0';
        const Predef *pd = predef_lookup(name);
        if (pd && pd->kind == PREDEF_INT) return pd->iv;
        if (pd) return 1;
        return 0;
    }
    p->i++;
    return 0;
}

static long long pexpr_rel(PExpr *p) {
    long long v = pexpr_primary(p);
    for (;;) {
        pexpr_skipws(p);
        const char *two = p->s + p->i;
        const char *op = NULL;
        if (p->i + 1 < p->n && (strncmp(two, "<=", 2) == 0 || strncmp(two, ">=", 2) == 0 ||
                                 strncmp(two, "==", 2) == 0 || strncmp(two, "!=", 2) == 0)) {
            op = two; p->i += 2;
        } else if (p->i < p->n && (p->s[p->i] == '<' || p->s[p->i] == '>')) {
            op = p->s + p->i; p->i += 1;
        } else break;
        long long r = pexpr_primary(p);
        if (strncmp(op, "<=", 2) == 0) v = v <= r;
        else if (strncmp(op, ">=", 2) == 0) v = v >= r;
        else if (strncmp(op, "==", 2) == 0) v = v == r;
        else if (strncmp(op, "!=", 2) == 0) v = v != r;
        else if (op[0] == '<') v = v < r;
        else v = v > r;
    }
    return v;
}

static long long pexpr_and(PExpr *p) {
    long long v = pexpr_rel(p);
    for (;;) {
        pexpr_skipws(p);
        if (p->i + 1 < p->n && strncmp(p->s + p->i, "&&", 2) == 0) { p->i += 2; v = (v && pexpr_rel(p)) ? 1 : 0; }
        else break;
    }
    return v;
}

static long long pexpr_or(PExpr *p) {
    long long v = pexpr_and(p);
    for (;;) {
        pexpr_skipws(p);
        if (p->i + 1 < p->n && strncmp(p->s + p->i, "||", 2) == 0) { p->i += 2; v = (v || pexpr_and(p)) ? 1 : 0; }
        else break;
    }
    return v;
}

static long long eval_preproc_expr(const char *text) {
    PExpr p; p.s = text; p.i = 0; p.n = (int)strlen(text);
    return pexpr_or(&p) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Lexer                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *src;
    int n;
    int pos;
    int line;
    int col;
    DiagList *diags;
} Lex;

typedef struct { int active; int everActive; int parentActive; } CondFrame;

static char lx_peek(Lex *L, int off) {
    int p = L->pos + off;
    if (p >= L->n) return '\0';
    return L->src[p];
}

static char lx_advance(Lex *L) {
    if (L->pos >= L->n) return '\0';
    char c = L->src[L->pos++];
    if (c == '\n') { L->line++; L->col = 1; } else { L->col++; }
    return c;
}

static void tl_push(NovaTokenList *list, NovaToken tok) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        NovaToken *grown = (NovaToken *)realloc(list->items, sizeof(NovaToken) * (size_t)list->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        list->items = grown;
    }
    list->items[list->count++] = tok;
}

static NovaToken make_token(NovaTokenType type, const char *lexeme, int line, int col) {
    NovaToken t;
    memset(&t, 0, sizeof(NovaToken));
    t.type = type;
    strncpy(t.lexeme, lexeme, sizeof(t.lexeme) - 1);
    t.line = line;
    t.column = col;
    return t;
}

static NovaTokenType keyword_lookup(const char *word) {
    static const struct { const char *w; NovaTokenType t; } KW[] = {
        { "auto", TOK_AUTO }, { "break", TOK_BREAK }, { "case", TOK_CASE }, { "char", TOK_CHAR },
        { "const", TOK_CONST }, { "continue", TOK_CONTINUE }, { "default", TOK_DEFAULT }, { "do", TOK_DO },
        { "double", TOK_DOUBLE }, { "else", TOK_ELSE }, { "enum", TOK_ENUM }, { "extern", TOK_EXTERN },
        { "float", TOK_FLOAT }, { "for", TOK_FOR }, { "goto", TOK_GOTO }, { "if", TOK_IF },
        { "int", TOK_INT }, { "long", TOK_LONG }, { "register", TOK_REGISTER }, { "return", TOK_RETURN },
        { "short", TOK_SHORT }, { "signed", TOK_SIGNED }, { "sizeof", TOK_SIZEOF }, { "static", TOK_STATIC },
        { "struct", TOK_STRUCT }, { "switch", TOK_SWITCH }, { "typedef", TOK_TYPEDEF }, { "union", TOK_UNION },
        { "unsigned", TOK_UNSIGNED }, { "void", TOK_VOID }, { "volatile", TOK_VOLATILE }, { "while", TOK_WHILE },
    };
    for (size_t i = 0; i < sizeof(KW) / sizeof(KW[0]); i++) {
        if (strcmp(KW[i].w, word) == 0) return KW[i].t;
    }
    return TOK_IDENTIFIER;
}

/* handle a conditional directive; condStack updated in place */
static void handle_conditional(Lex *L, const char *word, const char *rest,
                               CondFrame *stack, int *depth, int line, int col) {
    if (strcmp(word, "ifdef") == 0 || strcmp(word, "ifndef") == 0) {
        char name[128] = { 0 };
        int i = 0;
        while (rest[i] && isspace((unsigned char)rest[i])) i++;
        int j = 0;
        while (rest[i] && !isspace((unsigned char)rest[i]) && j < 127) name[j++] = rest[i++];
        name[j] = '\0';
        int defined = predef_lookup(name) ? 1 : 0;
        int cond = (strcmp(word, "ifdef") == 0) ? defined : !defined;
        int parent = (*depth == 0) ? 1 : stack[*depth - 1].active;
        if (*depth < 64) {
            stack[*depth].active = parent && cond;
            stack[*depth].everActive = parent && cond;
            stack[*depth].parentActive = parent;
            (*depth)++;
        }
    } else if (strcmp(word, "if") == 0) {
        int parent = (*depth == 0) ? 1 : stack[*depth - 1].active;
        int cond = parent ? (eval_preproc_expr(rest) != 0) : 0;
        if (*depth < 64) {
            stack[*depth].active = cond;
            stack[*depth].everActive = cond;
            stack[*depth].parentActive = parent;
            (*depth)++;
        }
    } else if (strcmp(word, "elif") == 0) {
        if (*depth == 0) { diag_add(L->diags, "error", line, col, "#elif without #if"); return; }
        CondFrame *top = &stack[*depth - 1];
        if (top->everActive || !top->parentActive) {
            top->active = 0;
        } else {
            top->active = eval_preproc_expr(rest) != 0;
            if (top->active) top->everActive = 1;
        }
    } else if (strcmp(word, "else") == 0) {
        if (*depth == 0) { diag_add(L->diags, "error", line, col, "#else without #if"); return; }
        CondFrame *top = &stack[*depth - 1];
        top->active = top->parentActive && !top->everActive;
        if (top->active) top->everActive = 1;
    } else if (strcmp(word, "endif") == 0) {
        if (*depth == 0) { diag_add(L->diags, "error", line, col, "#endif without #if"); return; }
        (*depth)--;
    }
}

NovaTokenList *nova_tokenize(const char *source, DiagList *diags) {
    Lex L;
    L.src = source;
    L.n = (int)strlen(source);
    L.pos = 0;
    L.line = 1;
    L.col = 1;
    L.diags = diags;

    NovaTokenList *list = (NovaTokenList *)calloc(1, sizeof(NovaTokenList));
    if (!list) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    list->capacity = 256;
    list->items = (NovaToken *)malloc(sizeof(NovaToken) * (size_t)list->capacity);
    if (!list->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }

    CondFrame stack[64];
    int depth = 0;
    int region_active_cache = 1;
    (void)region_active_cache;

    while (L.pos < L.n) {
        char c = lx_peek(&L, 0);

        /* Translation phase 2: backslash-newline line splicing */
        if (c == '\\' && (lx_peek(&L, 1) == '\n' || (lx_peek(&L, 1) == '\r' && lx_peek(&L, 2) == '\n'))) {
            lx_advance(&L); /* backslash */
            if (lx_peek(&L, 0) == '\r') lx_advance(&L);
            L.pos++; /* consume newline without counting a logical line */
            continue;
        }

        if (c == '\n' || c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            lx_advance(&L);
            continue;
        }

        if (c == '/' && lx_peek(&L, 1) == '/') {
            while (L.pos < L.n && lx_peek(&L, 0) != '\n') lx_advance(&L);
            continue;
        }
        if (c == '/' && lx_peek(&L, 1) == '*') {
            lx_advance(&L); lx_advance(&L);
            int closed = 0;
            while (L.pos < L.n) {
                if (lx_peek(&L, 0) == '*' && lx_peek(&L, 1) == '/') { lx_advance(&L); lx_advance(&L); closed = 1; break; }
                lx_advance(&L);
            }
            if (!closed) diag_add(diags, "error", L.line, L.col, "Unterminated block comment");
            continue;
        }

        int startLine = L.line, startCol = L.col;

        /* region activeness */
        int regionActive = 1;
        for (int i = 0; i < depth; i++) if (!stack[i].active) { regionActive = 0; break; }

        if (c == '#') {
            lx_advance(&L);
            char word[32] = { 0 };
            int wi = 0;
            while (L.pos < L.n && isalpha((unsigned char)lx_peek(&L, 0)) && wi < 31) word[wi++] = lx_advance(&L);
            word[wi] = '\0';

            if (strcmp(word, "ifdef") == 0 || strcmp(word, "ifndef") == 0 || strcmp(word, "if") == 0 ||
                strcmp(word, "elif") == 0 || strcmp(word, "else") == 0 || strcmp(word, "endif") == 0) {
                char rest[512] = { 0 };
                int ri = 0;
                while (L.pos < L.n && lx_peek(&L, 0) != '\n' && ri < 511) rest[ri++] = lx_advance(&L);
                rest[ri] = '\0';
                handle_conditional(&L, word, rest, stack, &depth, startLine, startCol);
                continue;
            }

            if (!regionActive) {
                while (L.pos < L.n && lx_peek(&L, 0) != '\n') lx_advance(&L);
                continue;
            }
            char full[40];
            snprintf(full, sizeof(full), "#%s", word);
            if (strcmp(full, "#include") == 0) tl_push(list, make_token(TOK_INCLUDE, full, startLine, startCol));
            else if (strcmp(full, "#define") == 0) tl_push(list, make_token(TOK_DEFINE, full, startLine, startCol));
            else tl_push(list, make_token(TOK_HASH, (full[1] ? full : "#"), startLine, startCol));
            continue;
        }

        if (!regionActive) {
            lx_advance(&L);
            continue;
        }

        /* identifiers / keywords / predefined macros */
        if (isalpha((unsigned char)c) || c == '_') {
            char lex[256] = { 0 };
            int li = 0;
            while (L.pos < L.n && (isalnum((unsigned char)lx_peek(&L, 0)) || lx_peek(&L, 0) == '_') && li < 255) {
                lex[li++] = lx_advance(&L);
            }
            lex[li] = '\0';

            if (strcmp(lex, "__attribute__") == 0) {
                if (lx_peek(&L, 0) == '(') {
                    int d2 = 0;
                    while (L.pos < L.n) {
                        char ch = lx_peek(&L, 0);
                        if (ch == '(') d2++;
                        else if (ch == ')') { d2--; if (d2 == 0) { lx_advance(&L); break; } }
                        else if (ch == '\n') break;
                        lx_advance(&L);
                    }
                }
                continue;
            }

            const Predef *pd = predef_lookup(lex);
            if (pd) {
                if (pd->kind == PREDEF_INT) {
                    char buf[32]; snprintf(buf, sizeof(buf), "%lld", pd->iv);
                    NovaToken t = make_token(TOK_INTEGER_LITERAL, buf, startLine, startCol);
                    t.int_value = pd->iv;
                    tl_push(list, t);
                } else if (pd->kind == PREDEF_FLOAT) {
                    char buf[40]; snprintf(buf, sizeof(buf), "%.17g", pd->fv);
                    NovaToken t = make_token(TOK_FLOAT_LITERAL, buf, startLine, startCol);
                    t.float_value = pd->fv;
                    tl_push(list, t);
                } else {
                    char lexb[256]; snprintf(lexb, sizeof(lexb), "\"%s\"", pd->sv);
                    NovaToken t = make_token(TOK_STRING_LITERAL, lexb, startLine, startCol);
                    t.string_value = strdup(pd->sv);
                    tl_push(list, t);
                }
                continue;
            }

            tl_push(list, make_token(keyword_lookup(lex), lex, startLine, startCol));
            continue;
        }

        /* numbers */
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lx_peek(&L, 1)))) {
            char lex[256] = { 0 };
            int li = 0;
            int isFloat = 0;
            if (c == '0' && (lx_peek(&L, 1) == 'x' || lx_peek(&L, 1) == 'X')) {
                lex[li++] = lx_advance(&L);
                lex[li++] = lx_advance(&L);
                while (L.pos < L.n && isxdigit((unsigned char)lx_peek(&L, 0)) && li < 255) lex[li++] = lx_advance(&L);
                lex[li] = '\0';
                while (L.pos < L.n && strchr("fFlLuU", lx_peek(&L, 0))) lx_advance(&L);
                NovaToken t = make_token(TOK_INTEGER_LITERAL, lex, startLine, startCol);
                t.int_value = strtoll(lex, NULL, 16);
                tl_push(list, t);
                continue;
            }
            while (L.pos < L.n && isdigit((unsigned char)lx_peek(&L, 0)) && li < 255) lex[li++] = lx_advance(&L);
            if (lx_peek(&L, 0) == '.' && isdigit((unsigned char)lx_peek(&L, 1))) {
                isFloat = 1;
                lex[li++] = lx_advance(&L);
                while (L.pos < L.n && isdigit((unsigned char)lx_peek(&L, 0)) && li < 255) lex[li++] = lx_advance(&L);
            } else if (lx_peek(&L, 0) == '.' && li > 0 && lx_peek(&L, 1) != 'e' && lx_peek(&L, 1) != 'E' && lx_peek(&L, 1) != '.') {
                if (!(isalpha((unsigned char)lx_peek(&L, 1)) || lx_peek(&L, 1) == '_')) {
                    isFloat = 1;
                    lex[li++] = lx_advance(&L);
                }
            }
            if (lx_peek(&L, 0) == 'e' || lx_peek(&L, 0) == 'E') {
                int savePos = L.pos, saveLine = L.line, saveCol = L.col;
                char expbuf[4] = { 0 }; int ei = 0;
                expbuf[ei++] = lx_advance(&L);
                if (lx_peek(&L, 0) == '+' || lx_peek(&L, 0) == '-') expbuf[ei++] = lx_advance(&L);
                expbuf[ei] = '\0';
                if (isdigit((unsigned char)lx_peek(&L, 0))) {
                    isFloat = 1;
                    for (int k = 0; k < ei && li < 255; k++) lex[li++] = expbuf[k];
                    while (L.pos < L.n && isdigit((unsigned char)lx_peek(&L, 0)) && li < 255) lex[li++] = lx_advance(&L);
                } else {
                    L.pos = savePos; L.line = saveLine; L.col = saveCol;
                }
            }
            lex[li] = '\0';
            while (L.pos < L.n && strchr("fFlLuU", lx_peek(&L, 0))) lx_advance(&L);
            if (isFloat) {
                NovaToken t = make_token(TOK_FLOAT_LITERAL, lex, startLine, startCol);
                t.float_value = strtod(lex, NULL);
                tl_push(list, t);
            } else {
                NovaToken t = make_token(TOK_INTEGER_LITERAL, lex, startLine, startCol);
                t.int_value = strtoll(lex, NULL, 10);
                tl_push(list, t);
            }
            continue;
        }

        /* char literals */
        if (c == '\'') {
            lx_advance(&L);
            int value = 0;
            char body[128] = { 0 }; int bi = 0;
            int closed = 0;
            while (L.pos < L.n && lx_peek(&L, 0) != '\'' && lx_peek(&L, 0) != '\n') {
                char ch = lx_advance(&L);
                if (ch == '\\' && L.pos < L.n) {
                    char esc = lx_advance(&L);
                    if (esc == 'n') value = 10;
                    else if (esc == 't') value = 9;
                    else if (esc == '0') value = 0;
                    else if (esc == 'r') value = 13;
                    else if (esc == '\\') value = 92;
                    else if (esc == '\'') value = 39;
                    else value = (unsigned char)esc;
                    if (bi < 126) { body[bi++] = '\\'; body[bi++] = esc; }
                } else {
                    value = (unsigned char)ch;
                    if (bi < 127) body[bi++] = ch;
                }
            }
            if (L.pos < L.n && lx_peek(&L, 0) == '\'') { lx_advance(&L); closed = 1; }
            if (!closed) diag_add(diags, "error", startLine, startCol, "Unterminated character literal");
            char lexb[160];
            snprintf(lexb, sizeof(lexb), "'%s'", body);
            NovaToken t = make_token(TOK_CHAR_LITERAL, lexb, startLine, startCol);
            t.char_value = value;
            tl_push(list, t);
            continue;
        }

        /* string literals */
        if (c == '"') {
            lx_advance(&L);
            char *value = (char *)malloc(256); int vcap = 256, vlen = 0;
            char lex[1024] = { '"' , 0 }; int li = 1;
            int closed = 0;
            while (L.pos < L.n && lx_peek(&L, 0) != '"' && lx_peek(&L, 0) != '\n') {
                char ch = lx_advance(&L);
                if (ch == '\\' && L.pos < L.n) {
                    char esc = lx_advance(&L);
                    char dec;
                    if (esc == 'n') dec = '\n';
                    else if (esc == 't') dec = '\t';
                    else if (esc == '0') dec = '\0';
                    else if (esc == 'r') dec = '\r';
                    else dec = esc;
                    if (vlen + 1 >= vcap) { vcap *= 2; value = (char *)realloc(value, vcap); }
                    value[vlen++] = dec;
                    if (li + 2 < 1023) { lex[li++] = '\\'; lex[li++] = esc; }
                } else {
                    if (vlen + 1 >= vcap) { vcap *= 2; value = (char *)realloc(value, vcap); }
                    value[vlen++] = ch;
                    if (li + 1 < 1023) lex[li++] = ch;
                }
            }
            if (L.pos < L.n && lx_peek(&L, 0) == '"') { lx_advance(&L); if (li + 1 < 1023) lex[li++] = '"'; closed = 1; }
            if (!closed) diag_add(diags, "error", startLine, startCol, "Unterminated string literal");
            lex[li] = '\0';
            value[vlen] = '\0';
            NovaToken t = make_token(TOK_STRING_LITERAL, lex, startLine, startCol);
            t.string_value = value;
            tl_push(list, t);
            continue;
        }

        /* ellipsis */
        if (c == '.' && lx_peek(&L, 1) == '.' && lx_peek(&L, 2) == '.') {
            lx_advance(&L); lx_advance(&L); lx_advance(&L);
            tl_push(list, make_token(TOK_ELLIPSIS, "...", startLine, startCol));
            continue;
        }

        /* 3-char operators */
        {
            char three[4] = { c, lx_peek(&L, 1), lx_peek(&L, 2), '\0' };
            if (strcmp(three, "<<=") == 0) { lx_advance(&L); lx_advance(&L); lx_advance(&L); tl_push(list, make_token(TOK_LSHIFT_ASSIGN, three, startLine, startCol)); continue; }
            if (strcmp(three, ">>=") == 0) { lx_advance(&L); lx_advance(&L); lx_advance(&L); tl_push(list, make_token(TOK_RSHIFT_ASSIGN, three, startLine, startCol)); continue; }
        }

        /* 2-char operators */
        {
            char two[3] = { c, lx_peek(&L, 1), '\0' };
            NovaTokenType tt = TOK_ERROR; int matched = 1;
            if (strcmp(two, "++") == 0) tt = TOK_PLUS_PLUS;
            else if (strcmp(two, "--") == 0) tt = TOK_MINUS_MINUS;
            else if (strcmp(two, "==") == 0) tt = TOK_EQ;
            else if (strcmp(two, "!=") == 0) tt = TOK_NEQ;
            else if (strcmp(two, "<=") == 0) tt = TOK_LEQ;
            else if (strcmp(two, ">=") == 0) tt = TOK_GEQ;
            else if (strcmp(two, "&&") == 0) tt = TOK_AND;
            else if (strcmp(two, "||") == 0) tt = TOK_OR;
            else if (strcmp(two, "+=") == 0) tt = TOK_PLUS_ASSIGN;
            else if (strcmp(two, "-=") == 0) tt = TOK_MINUS_ASSIGN;
            else if (strcmp(two, "*=") == 0) tt = TOK_STAR_ASSIGN;
            else if (strcmp(two, "/=") == 0) tt = TOK_SLASH_ASSIGN;
            else if (strcmp(two, "%=") == 0) tt = TOK_PERCENT_ASSIGN;
            else if (strcmp(two, "->") == 0) tt = TOK_ARROW;
            else if (strcmp(two, "<<") == 0) tt = TOK_LSHIFT;
            else if (strcmp(two, ">>") == 0) tt = TOK_RSHIFT;
            else if (strcmp(two, "&=") == 0) tt = TOK_AND_ASSIGN;
            else if (strcmp(two, "|=") == 0) tt = TOK_OR_ASSIGN;
            else if (strcmp(two, "^=") == 0) tt = TOK_XOR_ASSIGN;
            else matched = 0;
            if (matched) { lx_advance(&L); lx_advance(&L); tl_push(list, make_token(tt, two, startLine, startCol)); continue; }
        }

        /* single-char tokens */
        {
            NovaTokenType tt = TOK_ERROR; int matched = 1;
            switch (c) {
                case '+': tt = TOK_PLUS; break;
                case '-': tt = TOK_MINUS; break;
                case '*': tt = TOK_STAR; break;
                case '/': tt = TOK_SLASH; break;
                case '%': tt = TOK_PERCENT; break;
                case '=': tt = TOK_ASSIGN; break;
                case '!': tt = TOK_NOT; break;
                case '<': tt = TOK_LT; break;
                case '>': tt = TOK_GT; break;
                case '&': tt = TOK_BIT_AND; break;
                case '|': tt = TOK_BIT_OR; break;
                case '^': tt = TOK_BIT_XOR; break;
                case '~': tt = TOK_BIT_NOT; break;
                case '?': tt = TOK_QUESTION; break;
                case ':': tt = TOK_COLON; break;
                case '(': tt = TOK_LPAREN; break;
                case ')': tt = TOK_RPAREN; break;
                case '{': tt = TOK_LBRACE; break;
                case '}': tt = TOK_RBRACE; break;
                case '[': tt = TOK_LBRACKET; break;
                case ']': tt = TOK_RBRACKET; break;
                case ';': tt = TOK_SEMICOLON; break;
                case ',': tt = TOK_COMMA; break;
                case '.': tt = TOK_DOT; break;
                default: matched = 0; break;
            }
            if (matched) {
                lx_advance(&L);
                char one[2] = { c, '\0' };
                tl_push(list, make_token(tt, one, startLine, startCol));
                continue;
            }
        }

        lx_advance(&L);
        diag_add(diags, "error", startLine, startCol, "Unexpected character '%c'", c);
        char one[2] = { c, '\0' };
        tl_push(list, make_token(TOK_ERROR, one, startLine, startCol));
    }

    while (depth > 0) {
        depth--;
        diag_add(diags, "error", L.line, L.col, "Unterminated #if/#ifdef (missing #endif)");
    }

    tl_push(list, make_token(TOK_EOF, "EOF", L.line, L.col));
    return list;
}

void nova_token_list_free(NovaTokenList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type == TOK_STRING_LITERAL) free(list->items[i].string_value);
    }
    free(list->items);
    free(list);
}

const char *nova_token_type_name(NovaTokenType type) {
    switch (type) {
        case TOK_AUTO: return "TOKEN_AUTO";
        case TOK_BREAK: return "TOKEN_BREAK";
        case TOK_CASE: return "TOKEN_CASE";
        case TOK_CHAR: return "TOKEN_CHAR";
        case TOK_CONST: return "TOKEN_CONST";
        case TOK_CONTINUE: return "TOKEN_CONTINUE";
        case TOK_DEFAULT: return "TOKEN_DEFAULT";
        case TOK_DO: return "TOKEN_DO";
        case TOK_DOUBLE: return "TOKEN_DOUBLE";
        case TOK_ELSE: return "TOKEN_ELSE";
        case TOK_ENUM: return "TOKEN_ENUM";
        case TOK_EXTERN: return "TOKEN_EXTERN";
        case TOK_FLOAT: return "TOKEN_FLOAT";
        case TOK_FOR: return "TOKEN_FOR";
        case TOK_GOTO: return "TOKEN_GOTO";
        case TOK_IF: return "TOKEN_IF";
        case TOK_INT: return "TOKEN_INT";
        case TOK_LONG: return "TOKEN_LONG";
        case TOK_REGISTER: return "TOKEN_REGISTER";
        case TOK_RETURN: return "TOKEN_RETURN";
        case TOK_SHORT: return "TOKEN_SHORT";
        case TOK_SIGNED: return "TOKEN_SIGNED";
        case TOK_SIZEOF: return "TOKEN_SIZEOF";
        case TOK_STATIC: return "TOKEN_STATIC";
        case TOK_STRUCT: return "TOKEN_STRUCT";
        case TOK_SWITCH: return "TOKEN_SWITCH";
        case TOK_TYPEDEF: return "TOKEN_TYPEDEF";
        case TOK_UNION: return "TOKEN_UNION";
        case TOK_UNSIGNED: return "TOKEN_UNSIGNED";
        case TOK_VOID: return "TOKEN_VOID";
        case TOK_VOLATILE: return "TOKEN_VOLATILE";
        case TOK_WHILE: return "TOKEN_WHILE";
        case TOK_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TOK_INTEGER_LITERAL: return "TOKEN_INTEGER_LITERAL";
        case TOK_FLOAT_LITERAL: return "TOKEN_FLOAT_LITERAL";
        case TOK_CHAR_LITERAL: return "TOKEN_CHAR_LITERAL";
        case TOK_STRING_LITERAL: return "TOKEN_STRING_LITERAL";
        case TOK_PLUS: return "TOKEN_PLUS";
        case TOK_MINUS: return "TOKEN_MINUS";
        case TOK_STAR: return "TOKEN_STAR";
        case TOK_SLASH: return "TOKEN_SLASH";
        case TOK_PERCENT: return "TOKEN_PERCENT";
        case TOK_PLUS_PLUS: return "TOKEN_PLUS_PLUS";
        case TOK_MINUS_MINUS: return "TOKEN_MINUS_MINUS";
        case TOK_ASSIGN: return "TOKEN_ASSIGN";
        case TOK_PLUS_ASSIGN: return "TOKEN_PLUS_ASSIGN";
        case TOK_MINUS_ASSIGN: return "TOKEN_MINUS_ASSIGN";
        case TOK_STAR_ASSIGN: return "TOKEN_STAR_ASSIGN";
        case TOK_SLASH_ASSIGN: return "TOKEN_SLASH_ASSIGN";
        case TOK_PERCENT_ASSIGN: return "TOKEN_PERCENT_ASSIGN";
        case TOK_AND_ASSIGN: return "TOKEN_AND_ASSIGN";
        case TOK_OR_ASSIGN: return "TOKEN_OR_ASSIGN";
        case TOK_XOR_ASSIGN: return "TOKEN_XOR_ASSIGN";
        case TOK_LSHIFT_ASSIGN: return "TOKEN_LSHIFT_ASSIGN";
        case TOK_RSHIFT_ASSIGN: return "TOKEN_RSHIFT_ASSIGN";
        case TOK_EQ: return "TOKEN_EQ";
        case TOK_NEQ: return "TOKEN_NEQ";
        case TOK_LT: return "TOKEN_LT";
        case TOK_GT: return "TOKEN_GT";
        case TOK_LEQ: return "TOKEN_LEQ";
        case TOK_GEQ: return "TOKEN_GEQ";
        case TOK_AND: return "TOKEN_AND";
        case TOK_OR: return "TOKEN_OR";
        case TOK_NOT: return "TOKEN_NOT";
        case TOK_BIT_AND: return "TOKEN_AMPERSAND";
        case TOK_BIT_OR: return "TOKEN_BIT_OR";
        case TOK_BIT_XOR: return "TOKEN_BIT_XOR";
        case TOK_BIT_NOT: return "TOKEN_BIT_NOT";
        case TOK_LSHIFT: return "TOKEN_LSHIFT";
        case TOK_RSHIFT: return "TOKEN_RSHIFT";
        case TOK_QUESTION: return "TOKEN_QUESTION";
        case TOK_COLON: return "TOKEN_COLON";
        case TOK_LPAREN: return "TOKEN_LPAREN";
        case TOK_RPAREN: return "TOKEN_RPAREN";
        case TOK_LBRACE: return "TOKEN_LBRACE";
        case TOK_RBRACE: return "TOKEN_RBRACE";
        case TOK_LBRACKET: return "TOKEN_LBRACKET";
        case TOK_RBRACKET: return "TOKEN_RBRACKET";
        case TOK_SEMICOLON: return "TOKEN_SEMICOLON";
        case TOK_COMMA: return "TOKEN_COMMA";
        case TOK_DOT: return "TOKEN_DOT";
        case TOK_ARROW: return "TOKEN_ARROW";
        case TOK_INCLUDE: return "TOKEN_INCLUDE";
        case TOK_DEFINE: return "TOKEN_DEFINE";
        case TOK_HASH: return "TOKEN_HASH";
        case TOK_ELLIPSIS: return "TOKEN_ELLIPSIS";
        case TOK_EOF: return "TOKEN_EOF";
        case TOK_ERROR: return "TOKEN_ERROR";
        default: return "TOKEN_ERROR";
    }
}

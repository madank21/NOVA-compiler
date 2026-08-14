#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* Node helpers                                                               */
/* ------------------------------------------------------------------------- */

static NovaNode *node_new(NovaNodeType type, int line) {
    NovaNode *n = (NovaNode *)calloc(1, sizeof(NovaNode));
    if (!n) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    n->type = type;
    n->line = line;
    n->child_capacity = 4;
    n->children = (NovaNode **)malloc(sizeof(NovaNode *) * (size_t)n->child_capacity);
    if (!n->children) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return n;
}

static void node_add_child(NovaNode *parent, NovaNode *child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        parent->child_capacity *= 2;
        NovaNode **grown = (NovaNode **)realloc(parent->children, sizeof(NovaNode *) * (size_t)parent->child_capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        parent->children = grown;
    }
    parent->children[parent->child_count++] = child;
}

void nova_ast_free(NovaNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) nova_ast_free(node->children[i]);
    free(node->children);
    if (node->has_string) free(node->string_val);
    free(node);
}

/* ------------------------------------------------------------------------- */
/* Parser state                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    NovaTokenList *tokens;
    int pos;
    DiagList *diags;
} P;

static NovaToken *p_peek(P *p) { return &p->tokens->items[p->pos]; }
static int p_check(P *p, NovaTokenType t) { return p_peek(p)->type == t; }
static NovaToken *p_advance(P *p) {
    NovaToken *t = p_peek(p);
    if (p->pos < p->tokens->count - 1) p->pos++;
    return t;
}
static int p_match(P *p, NovaTokenType t) {
    if (p_check(p, t)) { p_advance(p); return 1; }
    return 0;
}
static NovaToken *p_expect(P *p, NovaTokenType t, const char *what) {
    if (p_check(p, t)) return p_advance(p);
    NovaToken *tok = p_peek(p);
    diag_add(p->diags, "error", tok->line, tok->column, "Expected %s but found '%s'", what, tok->lexeme);
    return NULL;
}
static int is_type_token(P *p) {
    NovaTokenType t = p_peek(p)->type;
    return t == TOK_INT || t == TOK_FLOAT || t == TOK_DOUBLE || t == TOK_CHAR || t == TOK_VOID;
}
static NovaToken *p_at(P *p, int off) {
    int idx = p->pos + off;
    if (idx >= p->tokens->count) idx = p->tokens->count - 1;
    return &p->tokens->items[idx];
}

static void skip_to_next_line(P *p) {
    int l = p_peek(p)->line;
    while (!p_check(p, TOK_EOF) && p_peek(p)->line == l) p_advance(p);
}

static void skip_declaration(P *p) {
    int depth = 0;
    while (!p_check(p, TOK_EOF)) {
        NovaTokenType ty = p_peek(p)->type;
        if (ty == TOK_LPAREN || ty == TOK_LBRACKET || ty == TOK_LBRACE) { depth++; p_advance(p); continue; }
        if (ty == TOK_RPAREN || ty == TOK_RBRACKET) { if (depth > 0) depth--; p_advance(p); continue; }
        if (ty == TOK_RBRACE) { if (depth == 0) return; depth--; p_advance(p); continue; }
        if (ty == TOK_SEMICOLON && depth == 0) { p_advance(p); return; }
        p_advance(p);
    }
}

/* forward decls */
static NovaNode *parse_expression(P *p);
static NovaNode *parse_assignment(P *p);
static NovaNode *parse_statement(P *p, int inLoop, int inSwitch);
static void parse_type_spec(P *p, TypeSpec *out);

/* ------------------------------------------------------------------------- */
/* Type spec                                                                  */
/* ------------------------------------------------------------------------- */

static void parse_type_spec(P *p, TypeSpec *out) {
    memset(out, 0, sizeof(TypeSpec));
    int sawLong = 0, sawShort = 0, sawUnsigned = 0, sawSigned = 0;
    int base = -1; /* -1 none, 0 int, 1 float, 2 double, 3 char, 4 void */
    char structName[128] = { 0 };

    for (;;) {
        NovaToken *t = p_peek(p);
        if (t->type == TOK_STATIC) { p_advance(p); out->isStatic = 1; continue; }
        if (t->type == TOK_EXTERN || t->type == TOK_REGISTER) { p_advance(p); out->isExtern = 1; continue; }
        if (t->type == TOK_CONST || t->type == TOK_VOLATILE) { p_advance(p); continue; }
        if (t->type == TOK_IDENTIFIER && strcmp(t->lexeme, "inline") == 0) { p_advance(p); continue; }
        if (t->type == TOK_SIGNED) { p_advance(p); sawSigned = 1; continue; }
        if (t->type == TOK_UNSIGNED) { p_advance(p); sawUnsigned = 1; continue; }
        if (t->type == TOK_SHORT) { p_advance(p); sawShort = 1; continue; }
        if (t->type == TOK_LONG) { p_advance(p); sawLong++; continue; }
        if (t->type == TOK_INT) { p_advance(p); base = 0; continue; }
        if (t->type == TOK_FLOAT) { p_advance(p); base = 1; continue; }
        if (t->type == TOK_DOUBLE) { p_advance(p); base = 2; continue; }
        if (t->type == TOK_CHAR) { p_advance(p); base = 3; continue; }
        if (t->type == TOK_VOID) { p_advance(p); base = 4; continue; }
        if (t->type == TOK_STRUCT && base == -1) {
            p_advance(p);
            NovaToken *nameTok = p_expect(p, TOK_IDENTIFIER, "struct name");
            if (nameTok) strncpy(structName, nameTok->lexeme, sizeof(structName) - 1);
            else strcpy(structName, "<error>");
            out->hasBase = 1;
            out->baseKind = 3;
            strcpy(out->structName, structName);
            snprintf(out->typeNameBuf, sizeof(out->typeNameBuf), "struct %s", structName);
            out->typeName = out->typeNameBuf;
            goto ptrs;
        }
        break;
    }

    if (base == -1 && (sawUnsigned || sawSigned || sawShort || sawLong)) base = 0;

    out->hasBase = (base != -1) || structName[0] != '\0';
    if (base == 1 || base == 2) { out->baseKind = 1; strcpy(out->typeNameBuf, "double"); }
    else if (base == 4) { out->baseKind = 2; strcpy(out->typeNameBuf, "void"); }
    else if (base == 3) { out->baseKind = 4; strcpy(out->typeNameBuf, "int"); } /* char */
    else if (structName[0]) { out->baseKind = 3; snprintf(out->typeNameBuf, sizeof(out->typeNameBuf), "struct %s", structName); }
    else { out->baseKind = 0; strcpy(out->typeNameBuf, "int"); }
    out->typeName = out->typeNameBuf;

ptrs:
    out->ptr = 0;
    while (p_check(p, TOK_STAR)) { p_advance(p); out->ptr++; }
    if (out->ptr > 0) {
        char stars[40] = { 0 };
        for (int i = 0; i < out->ptr && i < 38; i++) stars[i] = '*';
        strncat(out->typeNameBuf, stars, sizeof(out->typeNameBuf) - strlen(out->typeNameBuf) - 1);
    }
}

static int size_of_type_spec(TypeSpec *spec) {
    if (spec->ptr > 0) return 8;
    switch (spec->baseKind) {
        case 0: return 4; /* int/short/long */
        case 1: return 8; /* double */
        case 2: return 1; /* void */
        case 3: return 4; /* struct (nominal) */
        case 4: return 1; /* char */
        default: return 4;
    }
}

/* ------------------------------------------------------------------------- */
/* Expressions                                                                */
/* ------------------------------------------------------------------------- */

static NovaNode *parse_primary(P *p);
static NovaNode *parse_postfix(P *p);
static NovaNode *parse_unary(P *p);

typedef struct { NovaTokenType type; const char *op; } BinOpEntry;

static NovaNode *binary_level(P *p, NovaNode *(*next)(P *p), const BinOpEntry *ops, int nops) {
    NovaNode *left = next(p);
    for (;;) {
        const char *found = NULL;
        for (int i = 0; i < nops; i++) {
            if (p_peek(p)->type == ops[i].type) { found = ops[i].op; break; }
        }
        if (!found) break;
        NovaToken *t = p_advance(p);
        NovaNode *node = node_new(NODE_BINARY_OP, t->line);
        strncpy(node->op, found, sizeof(node->op) - 1);
        node->has_op = 1;
        node_add_child(node, left);
        node_add_child(node, next(p));
        left = node;
    }
    return left;
}

static NovaNode *parse_multiplicative(P *p) {
    static const BinOpEntry ops[] = { { TOK_STAR, "*" }, { TOK_SLASH, "/" }, { TOK_PERCENT, "%" } };
    return binary_level(p, parse_unary, ops, 3);
}
static NovaNode *parse_additive(P *p) {
    static const BinOpEntry ops[] = { { TOK_PLUS, "+" }, { TOK_MINUS, "-" } };
    return binary_level(p, parse_multiplicative, ops, 2);
}
static NovaNode *parse_shift(P *p) {
    static const BinOpEntry ops[] = { { TOK_LSHIFT, "<<" }, { TOK_RSHIFT, ">>" } };
    return binary_level(p, parse_additive, ops, 2);
}
static NovaNode *parse_relational(P *p) {
    static const BinOpEntry ops[] = { { TOK_LT, "<" }, { TOK_GT, ">" }, { TOK_LEQ, "<=" }, { TOK_GEQ, ">=" } };
    return binary_level(p, parse_shift, ops, 4);
}
static NovaNode *parse_equality(P *p) {
    static const BinOpEntry ops[] = { { TOK_EQ, "==" }, { TOK_NEQ, "!=" } };
    return binary_level(p, parse_relational, ops, 2);
}
static NovaNode *parse_bit_and(P *p) {
    static const BinOpEntry ops[] = { { TOK_BIT_AND, "&" } };
    return binary_level(p, parse_equality, ops, 1);
}
static NovaNode *parse_bit_xor(P *p) {
    static const BinOpEntry ops[] = { { TOK_BIT_XOR, "^" } };
    return binary_level(p, parse_bit_and, ops, 1);
}
static NovaNode *parse_bit_or(P *p) {
    static const BinOpEntry ops[] = { { TOK_BIT_OR, "|" } };
    return binary_level(p, parse_bit_xor, ops, 1);
}
static NovaNode *parse_logical_and(P *p) {
    static const BinOpEntry ops[] = { { TOK_AND, "&&" } };
    return binary_level(p, parse_bit_or, ops, 1);
}
static NovaNode *parse_logical_or(P *p) {
    static const BinOpEntry ops[] = { { TOK_OR, "||" } };
    return binary_level(p, parse_logical_and, ops, 1);
}

static NovaNode *parse_ternary(P *p) {
    NovaNode *cond = parse_logical_or(p);
    if (p_check(p, TOK_QUESTION)) {
        NovaToken *t = p_advance(p);
        NovaNode *node = node_new(NODE_TERNARY, t->line);
        node_add_child(node, cond);
        node_add_child(node, parse_assignment(p));
        p_expect(p, TOK_COLON, "':' in ternary expression");
        node_add_child(node, parse_ternary(p));
        return node;
    }
    return cond;
}

static NovaNode *parse_assignment(P *p) {
    NovaNode *cond = parse_ternary(p);
    NovaToken *t = p_peek(p);
    const char *op = NULL;
    switch (t->type) {
        case TOK_ASSIGN: op = "="; break;
        case TOK_PLUS_ASSIGN: op = "+="; break;
        case TOK_MINUS_ASSIGN: op = "-="; break;
        case TOK_STAR_ASSIGN: op = "*="; break;
        case TOK_SLASH_ASSIGN: op = "/="; break;
        case TOK_PERCENT_ASSIGN: op = "%="; break;
        case TOK_AND_ASSIGN: op = "&="; break;
        case TOK_OR_ASSIGN: op = "|="; break;
        case TOK_XOR_ASSIGN: op = "^="; break;
        case TOK_LSHIFT_ASSIGN: op = "<<="; break;
        case TOK_RSHIFT_ASSIGN: op = ">>="; break;
        default: break;
    }
    if (op) {
        p_advance(p);
        NovaNode *right = parse_assignment(p);
        NovaNode *node = node_new(strcmp(op, "=") == 0 ? NODE_ASSIGNMENT : NODE_COMPOUND_ASSIGN, t->line);
        strncpy(node->op, op, sizeof(node->op) - 1);
        node->has_op = 1;
        node_add_child(node, cond);
        node_add_child(node, right);
        return node;
    }
    return cond;
}

static NovaNode *parse_expression(P *p) { return parse_assignment(p); }

static int is_type_ahead(P *p) {
    NovaToken *t = p_at(p, 1);
    switch (t->type) {
        case TOK_INT: case TOK_FLOAT: case TOK_DOUBLE: case TOK_CHAR: case TOK_VOID:
        case TOK_SHORT: case TOK_LONG: case TOK_UNSIGNED: case TOK_SIGNED:
        case TOK_CONST: case TOK_VOLATILE: case TOK_STRUCT:
            return 1;
        default: return 0;
    }
}

static NovaNode *parse_cast(P *p) {
    NovaToken *t = p_advance(p); /* '(' */
    TypeSpec spec;
    parse_type_spec(p, &spec);
    p_expect(p, TOK_RPAREN, "')' after cast type");
    NovaNode *node = node_new(NODE_CAST, t->line);
    strncpy(node->type_name, spec.typeName, sizeof(node->type_name) - 1);
    node->has_type_name = 1;
    node_add_child(node, parse_unary(p));
    return node;
}

static NovaNode *parse_sizeof(P *p) {
    NovaToken *t = p_advance(p); /* 'sizeof' */
    if (p_check(p, TOK_LPAREN) && is_type_ahead(p)) {
        p_advance(p); /* '(' */
        TypeSpec spec;
        parse_type_spec(p, &spec);
        p_expect(p, TOK_RPAREN, "')' after sizeof type");
        NovaNode *lit = node_new(NODE_INT_LITERAL, t->line);
        lit->num_val = size_of_type_spec(&spec);
        lit->has_num = 1;
        return lit;
    }
    NovaNode *node = node_new(NODE_SIZEOF, t->line);
    node_add_child(node, parse_unary(p));
    return node;
}

static NovaNode *parse_unary(P *p) {
    NovaToken *t = p_peek(p);
    const char *op = NULL;
    switch (t->type) {
        case TOK_MINUS: op = "-"; break;
        case TOK_NOT: op = "!"; break;
        case TOK_STAR: op = "*"; break;
        case TOK_BIT_AND: op = "&"; break;
        case TOK_PLUS_PLUS: op = "++"; break;
        case TOK_MINUS_MINUS: op = "--"; break;
        case TOK_BIT_NOT: op = "~"; break;
        default: break;
    }
    if (op) {
        p_advance(p);
        NovaNode *node = node_new(NODE_UNARY_OP, t->line);
        strncpy(node->op, op, sizeof(node->op) - 1);
        node->has_op = 1;
        node_add_child(node, parse_unary(p));
        return node;
    }
    if (t->type == TOK_SIZEOF) return parse_sizeof(p);
    if (t->type == TOK_LPAREN && is_type_ahead(p)) return parse_cast(p);
    return parse_postfix(p);
}

static NovaNode *parse_postfix(P *p) {
    NovaNode *expr = parse_primary(p);
    for (;;) {
        NovaToken *t = p_peek(p);
        if (t->type == TOK_LPAREN && expr->type == NODE_IDENTIFIER && strcmp(expr->identifier, "offsetof") == 0) {
            diag_add(p->diags, "error", expr->line, 1, "'offsetof' is not supported in the NOVA C subset");
            p_advance(p); /* '(' */
            int depth = 1;
            while (!p_check(p, TOK_EOF) && depth > 0) {
                if (p_check(p, TOK_LPAREN)) depth++;
                else if (p_check(p, TOK_RPAREN)) depth--;
                p_advance(p);
            }
            NovaNode *lit = node_new(NODE_INT_LITERAL, expr->line);
            lit->num_val = 0;
            lit->has_num = 1;
            nova_ast_free(expr);
            expr = lit;
            continue;
        }
        if (t->type == TOK_LPAREN) {
            p_advance(p);
            NovaNode *call = node_new(NODE_FUNC_CALL, t->line);
            if (expr->type == NODE_IDENTIFIER) {
                strncpy(call->identifier, expr->identifier, sizeof(call->identifier) - 1);
                call->has_identifier = 1;
            } else {
                strcpy(call->identifier, "<expr>");
                call->has_identifier = 1;
            }
            if (!p_check(p, TOK_RPAREN)) {
                for (;;) {
                    node_add_child(call, parse_expression(p));
                    if (p_match(p, TOK_COMMA)) continue;
                    break;
                }
            }
            p_expect(p, TOK_RPAREN, "')' after arguments");
            nova_ast_free(expr);
            expr = call;
        } else if (t->type == TOK_LBRACKET) {
            p_advance(p);
            NovaNode *idx = node_new(NODE_INDEX, t->line);
            node_add_child(idx, expr);
            node_add_child(idx, parse_expression(p));
            p_expect(p, TOK_RBRACKET, "']' after index");
            expr = idx;
        } else if (t->type == TOK_DOT || t->type == TOK_ARROW) {
            p_advance(p);
            NovaNode *mem = node_new(NODE_MEMBER, t->line);
            node_add_child(mem, expr);
            NovaToken *id = p_expect(p, TOK_IDENTIFIER, "field name after \".\"");
            if (id) { strncpy(mem->identifier, id->lexeme, sizeof(mem->identifier) - 1); mem->has_identifier = 1; }
            else { strcpy(mem->identifier, "<error>"); mem->has_identifier = 1; }
            expr = mem;
        } else if (t->type == TOK_PLUS_PLUS || t->type == TOK_MINUS_MINUS) {
            p_advance(p);
            NovaNode *node = node_new(NODE_UNARY_OP, t->line);
            strcpy(node->op, t->type == TOK_PLUS_PLUS ? "p++" : "p--");
            node->has_op = 1;
            node_add_child(node, expr);
            expr = node;
        } else {
            break;
        }
    }
    return expr;
}

static NovaNode *parse_primary(P *p) {
    NovaToken *t = p_peek(p);
    if (t->type == TOK_INTEGER_LITERAL) {
        p_advance(p);
        NovaNode *node = node_new(NODE_INT_LITERAL, t->line);
        node->num_val = (double)t->int_value;
        node->has_num = 1;
        return node;
    }
    if (t->type == TOK_FLOAT_LITERAL) {
        p_advance(p);
        NovaNode *node = node_new(NODE_FLOAT_LITERAL, t->line);
        node->num_val = t->float_value;
        node->has_num = 1;
        return node;
    }
    if (t->type == TOK_CHAR_LITERAL) {
        p_advance(p);
        NovaNode *node = node_new(NODE_INT_LITERAL, t->line);
        node->num_val = (double)t->char_value;
        node->has_num = 1;
        return node;
    }
    if (t->type == TOK_STRING_LITERAL) {
        NovaToken *first = p_advance(p);
        char *value = strdup(first->string_value ? first->string_value : "");
        if (!value) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        /* adjacent string literal concatenation */
        while (p_check(p, TOK_STRING_LITERAL)) {
            NovaToken *nxt = p_advance(p);
            const char *add = nxt->string_value ? nxt->string_value : "";
            size_t oldlen = strlen(value), addlen = strlen(add);
            char *grown = (char *)realloc(value, oldlen + addlen + 1);
            if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
            memcpy(grown + oldlen, add, addlen + 1);
            value = grown;
        }
        NovaNode *node = node_new(NODE_STRING_LITERAL, t->line);
        node->string_val = value;
        node->has_string = 1;
        return node;
    }
    if (t->type == TOK_IDENTIFIER) {
        p_advance(p);
        NovaNode *node = node_new(NODE_IDENTIFIER, t->line);
        strncpy(node->identifier, t->lexeme, sizeof(node->identifier) - 1);
        node->has_identifier = 1;
        return node;
    }
    if (t->type == TOK_LPAREN) {
        p_advance(p);
        NovaNode *inner = parse_expression(p);
        p_expect(p, TOK_RPAREN, "')'");
        return inner;
    }
    diag_add(p->diags, "error", t->line, t->column, "Unexpected token '%s' in expression", t->lexeme);
    p_advance(p);
    return node_new(NODE_ERROR, t->line);
}

/* ------------------------------------------------------------------------- */
/* Declarations & statements                                                  */
/* ------------------------------------------------------------------------- */

static NovaNode *parse_single_declarator(P *p, const char *typeName, int line, NovaToken *firstIdTok) {
    NovaNode *decl = node_new(NODE_VAR_DECL, line);
    char type[160];
    strncpy(type, typeName, sizeof(type) - 8);
    type[sizeof(type) - 8] = '\0';
    if (!firstIdTok) {
        while (p_check(p, TOK_STAR)) { p_advance(p); strncat(type, "*", sizeof(type) - strlen(type) - 1); }
    }
    NovaToken *id = firstIdTok ? firstIdTok : p_expect(p, TOK_IDENTIFIER, "variable name");
    if (id) { strncpy(decl->identifier, id->lexeme, sizeof(decl->identifier) - 1); decl->has_identifier = 1; }
    else { strcpy(decl->identifier, "<error>"); decl->has_identifier = 1; }
    strncpy(decl->type_name, type, sizeof(decl->type_name) - 1);
    decl->has_type_name = 1;

    if (p_match(p, TOK_LBRACKET)) {
        decl->is_array = 1;
        if (p_check(p, TOK_INTEGER_LITERAL)) {
            NovaToken *sz = p_advance(p);
            NovaNode *nn = node_new(NODE_INT_LITERAL, sz->line);
            nn->num_val = (double)sz->int_value;
            nn->has_num = 1;
            node_add_child(decl, nn);
            decl->has_size = 1;
        }
        p_expect(p, TOK_RBRACKET, "']' after array size");
    }
    if (p_match(p, TOK_ASSIGN)) {
        if ((decl->is_array || strncmp(type, "struct ", 7) == 0) && p_check(p, TOK_LBRACE)) {
            p_advance(p);
            while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
                node_add_child(decl, parse_expression(p));
                if (p_match(p, TOK_COMMA)) continue;
                break;
            }
            p_expect(p, TOK_RBRACE, "'}' after initializer list");
        } else {
            node_add_child(decl, parse_expression(p));
        }
    }
    return decl;
}

static NovaNode *parse_var_decl_tail(P *p, const char *typeName, int line) {
    NovaNode *first = parse_single_declarator(p, typeName, line, NULL);
    if (!p_check(p, TOK_COMMA)) {
        p_expect(p, TOK_SEMICOLON, "';' after declaration");
        return first;
    }
    NovaNode *group = node_new(NODE_DECL_LIST, line);
    node_add_child(group, first);
    while (p_match(p, TOK_COMMA)) {
        node_add_child(group, parse_single_declarator(p, typeName, line, NULL));
    }
    p_expect(p, TOK_SEMICOLON, "';' after declaration");
    return group;
}

static int is_decl_type_start(P *p) {
    NovaToken *t = p_peek(p);
    switch (t->type) {
        case TOK_STATIC: case TOK_EXTERN: case TOK_REGISTER: case TOK_CONST:
        case TOK_VOLATILE: case TOK_UNSIGNED: case TOK_SIGNED: case TOK_SHORT: case TOK_LONG:
            return 1;
        default:
            return (t->type == TOK_IDENTIFIER && strcmp(t->lexeme, "inline") == 0);
    }
}

static NovaNode *parse_local_decl_from_spec(P *p, TypeSpec *spec) {
    NovaToken *startTok = p_peek(p);
    if (p_check(p, TOK_IDENTIFIER) && p_at(p, 1)->type == TOK_LPAREN) {
        NovaToken *afterParen = p_at(p, 2);
        int looksParam = 0;
        switch (afterParen->type) {
            case TOK_INT: case TOK_FLOAT: case TOK_DOUBLE: case TOK_CHAR: case TOK_VOID:
            case TOK_SHORT: case TOK_LONG: case TOK_UNSIGNED: case TOK_SIGNED:
            case TOK_CONST: case TOK_VOLATILE: case TOK_STRUCT:
            case TOK_RPAREN: case TOK_ELLIPSIS:
                looksParam = 1;
            default: break;
        }
        if (looksParam) {
            diag_add(p->diags, "error", startTok->line, startTok->column,
                "Nested function definitions are not supported in the NOVA C subset (they are a GNU C extension)");
            skip_declaration(p);
            return node_new(NODE_EMPTY, startTok->line);
        }
    }
    if (p_check(p, TOK_LPAREN) && p_at(p, 1)->type == TOK_STAR) {
        diag_add(p->diags, "error", p_peek(p)->line, p_peek(p)->column,
            "Function pointers are not supported in the NOVA C subset");
        skip_declaration(p);
        return node_new(NODE_EMPTY, startTok->line);
    }

    int line = startTok->line;
    NovaNode *first = parse_single_declarator(p, spec->typeName, line, NULL);
    if (spec->isStatic) first->is_static = 1;
    if (!p_check(p, TOK_COMMA)) {
        p_expect(p, TOK_SEMICOLON, "';' after declaration");
        return first;
    }
    NovaNode *group = node_new(NODE_DECL_LIST, line);
    node_add_child(group, first);
    while (p_match(p, TOK_COMMA)) {
        NovaNode *d = parse_single_declarator(p, spec->typeName, line, NULL);
        if (spec->isStatic) d->is_static = 1;
        node_add_child(group, d);
    }
    p_expect(p, TOK_SEMICOLON, "';' after declaration");
    return group;
}

static NovaNode *parse_statement(P *p, int inLoop, int inSwitch) {
    NovaToken *t = p_peek(p);

    if (t->type == TOK_INCLUDE || t->type == TOK_DEFINE || t->type == TOK_HASH) {
        int l = t->line;
        while (!p_check(p, TOK_EOF) && p_peek(p)->line == l) p_advance(p);
        return node_new(NODE_EMPTY, t->line);
    }

    if (t->type == TOK_IF) {
        p_advance(p);
        NovaNode *node = node_new(NODE_IF_STMT, t->line);
        p_expect(p, TOK_LPAREN, "'(' after 'if'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOK_RPAREN, "')' after condition");
        node_add_child(node, parse_statement(p, inLoop, inSwitch));
        if (p_match(p, TOK_ELSE)) node_add_child(node, parse_statement(p, inLoop, inSwitch));
        return node;
    }
    if (t->type == TOK_WHILE) {
        p_advance(p);
        NovaNode *node = node_new(NODE_WHILE_STMT, t->line);
        p_expect(p, TOK_LPAREN, "'(' after 'while'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOK_RPAREN, "')' after condition");
        node_add_child(node, parse_statement(p, 1, inSwitch));
        return node;
    }
    if (t->type == TOK_DO) {
        p_advance(p);
        NovaNode *node = node_new(NODE_DO_WHILE_STMT, t->line);
        node_add_child(node, parse_statement(p, 1, inSwitch));
        p_expect(p, TOK_WHILE, "'while' after do-body");
        p_expect(p, TOK_LPAREN, "'(' after 'while'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOK_RPAREN, "')' after do-while condition");
        p_expect(p, TOK_SEMICOLON, "';' after do-while");
        return node;
    }
    if (t->type == TOK_SWITCH) {
        p_advance(p);
        NovaNode *node = node_new(NODE_SWITCH_STMT, t->line);
        p_expect(p, TOK_LPAREN, "'(' after 'switch'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOK_RPAREN, "')' after switch expression");
        p_expect(p, TOK_LBRACE, "'{' after switch expression");
        while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
            if (p_check(p, TOK_CASE)) {
                NovaToken *ct = p_advance(p);
                NovaNode *caseNode = node_new(NODE_CASE, ct->line);
                int sign = 1;
                if (p_check(p, TOK_MINUS)) { p_advance(p); sign = -1; }
                if (p_check(p, TOK_INTEGER_LITERAL) || p_check(p, TOK_CHAR_LITERAL)) {
                    NovaToken *v = p_advance(p);
                    long long val = (v->type == TOK_CHAR_LITERAL) ? v->char_value : v->int_value;
                    caseNode->num_val = (double)(sign * val);
                    caseNode->has_num = 1;
                } else {
                    diag_add(p->diags, "error", ct->line, ct->column,
                        "Case value must be an integer constant in the NOVA subset");
                }
                p_expect(p, TOK_COLON, "':' after case value");
                while (!p_check(p, TOK_CASE) && !p_check(p, TOK_DEFAULT) &&
                       !p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
                    node_add_child(caseNode, parse_statement(p, inLoop, 1));
                }
                node_add_child(node, caseNode);
            } else if (p_check(p, TOK_DEFAULT)) {
                NovaToken *dt = p_advance(p);
                NovaNode *defNode = node_new(NODE_DEFAULT, dt->line);
                p_expect(p, TOK_COLON, "':' after 'default'");
                while (!p_check(p, TOK_CASE) && !p_check(p, TOK_DEFAULT) &&
                       !p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
                    node_add_child(defNode, parse_statement(p, inLoop, 1));
                }
                node_add_child(node, defNode);
            } else {
                NovaToken *tok = p_peek(p);
                diag_add(p->diags, "error", tok->line, tok->column,
                    "Expected 'case' or 'default' in switch but found '%s'", tok->lexeme);
                p_advance(p);
            }
        }
        p_expect(p, TOK_RBRACE, "'}' to close switch");
        return node;
    }
    if (t->type == TOK_GOTO) {
        p_advance(p);
        NovaNode *node = node_new(NODE_GOTO, t->line);
        NovaToken *id = p_expect(p, TOK_IDENTIFIER, "label name after goto");
        if (id) { strncpy(node->identifier, id->lexeme, sizeof(node->identifier) - 1); node->has_identifier = 1; }
        else { strcpy(node->identifier, "<error>"); node->has_identifier = 1; }
        p_expect(p, TOK_SEMICOLON, "';' after goto");
        return node;
    }
    if (t->type == TOK_FOR) {
        p_advance(p);
        NovaNode *node = node_new(NODE_FOR_STMT, t->line);
        p_expect(p, TOK_LPAREN, "'(' after 'for'");
        if (p_check(p, TOK_SEMICOLON)) {
            p_advance(p);
            node_add_child(node, node_new(NODE_EMPTY, t->line));
        } else if (is_type_token(p) || is_decl_type_start(p)) {
            TypeSpec spec;
            parse_type_spec(p, &spec);
            node_add_child(node, parse_var_decl_tail(p, spec.typeName, t->line));
        } else {
            NovaNode *e = node_new(NODE_EXPRESSION_STMT, t->line);
            node_add_child(e, parse_expression(p));
            p_expect(p, TOK_SEMICOLON, "';' after for-loop initializer");
            node_add_child(node, e);
        }
        if (p_check(p, TOK_SEMICOLON)) node_add_child(node, node_new(NODE_EMPTY, t->line));
        else node_add_child(node, parse_expression(p));
        p_expect(p, TOK_SEMICOLON, "';' after for-loop condition");
        if (p_check(p, TOK_RPAREN)) node_add_child(node, node_new(NODE_EMPTY, t->line));
        else node_add_child(node, parse_expression(p));
        p_expect(p, TOK_RPAREN, "')' after for-loop clauses");
        node_add_child(node, parse_statement(p, 1, inSwitch));
        return node;
    }
    if (t->type == TOK_RETURN) {
        p_advance(p);
        NovaNode *node = node_new(NODE_RETURN_STMT, t->line);
        if (!p_check(p, TOK_SEMICOLON)) node_add_child(node, parse_expression(p));
        p_expect(p, TOK_SEMICOLON, "';' after return");
        return node;
    }
    if (t->type == TOK_BREAK) {
        p_advance(p);
        p_expect(p, TOK_SEMICOLON, "';' after break");
        return node_new(NODE_BREAK_STMT, t->line);
    }
    if (t->type == TOK_CONTINUE) {
        p_advance(p);
        p_expect(p, TOK_SEMICOLON, "';' after continue");
        return node_new(NODE_CONTINUE_STMT, t->line);
    }
    if (t->type == TOK_LBRACE) {
        p_advance(p);
        NovaNode *comp = node_new(NODE_COMPOUND_STMT, t->line);
        while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
            node_add_child(comp, parse_statement(p, inLoop, inSwitch));
        }
        p_expect(p, TOK_RBRACE, "'}' to close block");
        return comp;
    }
    /* Label: name: statement */
    if (t->type == TOK_IDENTIFIER && p_at(p, 1)->type == TOK_COLON) {
        p_advance(p);
        NovaNode *labelNode = node_new(NODE_LABEL_STMT, t->line);
        strncpy(labelNode->identifier, t->lexeme, sizeof(labelNode->identifier) - 1);
        labelNode->has_identifier = 1;
        p_advance(p); /* ':' */
        node_add_child(labelNode, parse_statement(p, inLoop, inSwitch));
        return labelNode;
    }
    if (is_type_token(p) || is_decl_type_start(p)) {
        TypeSpec spec;
        parse_type_spec(p, &spec);
        if (!spec.hasBase) {
            /* only qualifiers seen — fall through to expression */
        } else if (p_check(p, TOK_IDENTIFIER) && p_at(p, 1)->type == TOK_IDENTIFIER) {
            diag_add(p->diags, "error", t->line, t->column,
                "Unsupported type combination near '%s' (e.g. complex numbers are not in the NOVA subset)",
                p_peek(p)->lexeme);
            skip_declaration(p);
            return node_new(NODE_EMPTY, t->line);
        } else {
            return parse_local_decl_from_spec(p, &spec);
        }
    }
    if (t->type == TOK_UNION || t->type == TOK_ENUM) {
        diag_add(p->diags, "error", t->line, t->column, "'%s' is not supported in the NOVA C subset", t->lexeme);
        skip_declaration(p);
        return node_new(NODE_EMPTY, t->line);
    }
    if (t->type == TOK_IDENTIFIER &&
        (p_at(p, 1)->type == TOK_IDENTIFIER || p_at(p, 1)->type == TOK_STAR)) {
        diag_add(p->diags, "error", t->line, t->column,
            "Unknown type '%s' — typedef names are not supported in the NOVA C subset", t->lexeme);
        skip_declaration(p);
        return node_new(NODE_EMPTY, t->line);
    }
    if (t->type == TOK_STRUCT) {
        if (p_at(p, 1)->type == TOK_IDENTIFIER && p_at(p, 2)->type == TOK_LBRACE) {
            diag_add(p->diags, "error", t->line, t->column,
                "Local struct definitions are not supported in the NOVA C subset");
            skip_declaration(p);
            return node_new(NODE_EMPTY, t->line);
        }
        p_advance(p);
        NovaToken *nameTok = p_expect(p, TOK_IDENTIFIER, "struct name");
        char tn[384];
        snprintf(tn, sizeof(tn), "struct %s", nameTok ? nameTok->lexeme : "<error>");
        return parse_var_decl_tail(p, tn, t->line);
    }
    if (t->type == TOK_SEMICOLON) {
        p_advance(p);
        return node_new(NODE_EMPTY, t->line);
    }

    int before = p->pos;
    NovaNode *expr = parse_expression(p);
    if (p->pos == before) {
        diag_add(p->diags, "error", t->line, t->column, "Unexpected token '%s'", t->lexeme);
        p_advance(p);
        return node_new(NODE_ERROR, t->line);
    }
    NovaNode *stmt = node_new(NODE_EXPRESSION_STMT, t->line);
    node_add_child(stmt, expr);
    p_expect(p, TOK_SEMICOLON, "';' after expression");
    return stmt;
}

static void parse_parameter_list(P *p, NovaNode *func) {
    if (p_check(p, TOK_RPAREN)) return;
    if (p_check(p, TOK_VOID) && p_at(p, 1)->type == TOK_RPAREN) {
        p_advance(p); /* int main(void) */
        return;
    }
    for (;;) {
        if (p_check(p, TOK_ELLIPSIS)) {
            NovaToken *et = p_advance(p);
            diag_add(p->diags, "error", et->line, et->column,
                "Variadic functions (...) are not supported in the NOVA C subset");
            continue;
        }
        if (!is_type_token(p) && !is_decl_type_start(p)) {
            diag_add(p->diags, "error", p_peek(p)->line, p_peek(p)->column,
                "Expected parameter type but found '%s'", p_peek(p)->lexeme);
            break;
        }
        TypeSpec spec;
        parse_type_spec(p, &spec);
        NovaToken *id = p_expect(p, TOK_IDENTIFIER, "parameter name");
        NovaNode *param = node_new(NODE_PARAMETER, id ? id->line : func->line);
        strncpy(param->type_name, spec.typeName, sizeof(param->type_name) - 1);
        param->has_type_name = 1;
        if (id) { strncpy(param->identifier, id->lexeme, sizeof(param->identifier) - 1); param->has_identifier = 1; }
        else { strcpy(param->identifier, "<error>"); param->has_identifier = 1; }
        /* array parameter: int arr[] (optional size is accepted and ignored —
         * the parameter decays to a pointer, exactly like C) */
        if (p_check(p, TOK_LBRACKET)) {
            p_advance(p);
            while (!p_check(p, TOK_RBRACKET) && !p_check(p, TOK_EOF) && !p_check(p, TOK_COMMA)) p_advance(p);
            if (p_check(p, TOK_RBRACKET)) p_advance(p);
            param->is_array = 1;
        }
        node_add_child(func, param);
        if (p_match(p, TOK_COMMA)) continue;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Top level                                                                  */
/* ------------------------------------------------------------------------- */

NovaNode *nova_parse(NovaTokenList *tokens, DiagList *diags) {
    P parser;
    parser.tokens = tokens;
    parser.pos = 0;
    parser.diags = diags;
    P *p = &parser;

    NovaNode *root = node_new(NODE_PROGRAM, 1);

    while (!p_check(p, TOK_EOF)) {
        NovaToken *t = p_peek(p);

        if (t->type == TOK_INCLUDE || t->type == TOK_DEFINE || t->type == TOK_HASH) {
            skip_to_next_line(p);
            continue;
        }

        if (t->type == TOK_TYPEDEF) {
            p_advance(p);
            diag_add(diags, "error", t->line, t->column, "typedef is not supported in the NOVA C subset");
            skip_declaration(p);
            continue;
        }
        if (t->type == TOK_UNION || t->type == TOK_ENUM) {
            diag_add(diags, "error", t->line, t->column, "'%s' is not supported in the NOVA C subset", t->lexeme);
            skip_declaration(p);
            continue;
        }

        if (t->type == TOK_STRUCT) {
            p_advance(p);
            NovaToken *nameTok = p_expect(p, TOK_IDENTIFIER, "struct name");
            if (p_check(p, TOK_LBRACE)) {
                p_advance(p);
                NovaNode *def = node_new(NODE_STRUCT_DEF, t->line);
                if (nameTok) { strncpy(def->identifier, nameTok->lexeme, sizeof(def->identifier) - 1); def->has_identifier = 1; }
                else { strcpy(def->identifier, "<error>"); def->has_identifier = 1; }
                while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
                    NovaToken *ft = p_peek(p);
                    if (ft->type == TOK_UNION || ft->type == TOK_UNSIGNED ||
                        ft->type == TOK_SIGNED || ft->type == TOK_SHORT ||
                        ft->type == TOK_LONG) {
                        diag_add(diags, "error", ft->line, ft->column,
                            "'%s' struct fields are not supported in the NOVA C subset", ft->lexeme);
                        skip_declaration(p);
                        continue;
                    }
                    if (!is_type_token(p) && !is_decl_type_start(p) && p_peek(p)->type != TOK_STRUCT) {
                        diag_add(diags, "error", ft->line, ft->column,
                            "Expected field type in struct but found '%s'", ft->lexeme);
                        skip_declaration(p);
                        continue;
                    }
                    TypeSpec fspec;
                    parse_type_spec(p, &fspec);
                    NovaNode *field = node_new(NODE_STRUCT_FIELD, ft->line);
                    strncpy(field->type_name, fspec.typeName, sizeof(field->type_name) - 1);
                    field->has_type_name = 1;
                    NovaToken *fid = p_expect(p, TOK_IDENTIFIER, "field name");
                    if (fid) { strncpy(field->identifier, fid->lexeme, sizeof(field->identifier) - 1); field->has_identifier = 1; }
                    else { strcpy(field->identifier, "<error>"); field->has_identifier = 1; }
                    if (p_match(p, TOK_LBRACKET)) {
                        if (!p_check(p, TOK_INTEGER_LITERAL)) {
                            diag_add(diags, "error", p_peek(p)->line, p_peek(p)->column,
                                "Flexible array members are not supported in the NOVA C subset");
                            p_expect(p, TOK_RBRACKET, "']' after field size");
                        } else {
                            field->is_array = 1;
                            NovaToken *sz = p_advance(p);
                            NovaNode *nn = node_new(NODE_INT_LITERAL, sz->line);
                            nn->num_val = (double)sz->int_value;
                            nn->has_num = 1;
                            node_add_child(field, nn);
                            field->has_size = 1;
                            p_expect(p, TOK_RBRACKET, "']' after field size");
                        }
                    }
                    if (p_check(p, TOK_COLON)) {
                        p_advance(p);
                        diag_add(diags, "error", ft->line, ft->column, "Bitfields are not supported in the NOVA C subset");
                        if (p_check(p, TOK_INTEGER_LITERAL)) p_advance(p);
                    }
                    p_expect(p, TOK_SEMICOLON, "';' after struct field");
                    node_add_child(def, field);
                }
                p_expect(p, TOK_RBRACE, "'}' to close struct");
                p_expect(p, TOK_SEMICOLON, "';' after struct definition");
                node_add_child(root, def);
            } else {
                char tn[384];
                snprintf(tn, sizeof(tn), "struct %s", nameTok ? nameTok->lexeme : "<error>");
                node_add_child(root, parse_var_decl_tail(p, tn, t->line));
            }
            continue;
        }

        if (is_type_token(p) || is_decl_type_start(p)) {
            TypeSpec spec;
            parse_type_spec(p, &spec);
            if (!spec.hasBase) {
                diag_add(diags, "error", p_peek(p)->line, p_peek(p)->column,
                    "Expected a type in declaration near '%s'", p_peek(p)->lexeme);
                skip_declaration(p);
                continue;
            }
            if (p_check(p, TOK_LPAREN) && p_at(p, 1)->type == TOK_STAR) {
                diag_add(diags, "error", p_peek(p)->line, p_peek(p)->column,
                    "Function pointers are not supported in the NOVA C subset");
                skip_declaration(p);
                continue;
            }
            NovaToken *id = p_expect(p, TOK_IDENTIFIER, "function or variable name");
            if (!id) { skip_declaration(p); continue; }
            if (p_check(p, TOK_LPAREN)) {
                p_advance(p);
                NovaNode *func = node_new(NODE_FUNCTION_DEF, t->line);
                strncpy(func->type_name, spec.typeName, sizeof(func->type_name) - 1);
                func->has_type_name = 1;
                strncpy(func->identifier, id->lexeme, sizeof(func->identifier) - 1);
                func->has_identifier = 1;
                parse_parameter_list(p, func);
                p_expect(p, TOK_RPAREN, "')' after parameters");
                if (p_match(p, TOK_SEMICOLON)) {
                    func->is_forward = 1;
                    node_add_child(root, func);
                    continue;
                }
                if (!p_check(p, TOK_LBRACE)) {
                    diag_add(diags, "error", p_peek(p)->line, p_peek(p)->column, "Expected '{' after function signature");
                    skip_declaration(p);
                    node_add_child(root, func);
                    continue;
                }
                node_add_child(func, parse_statement(p, 0, 0));
                node_add_child(root, func);
            } else {
                NovaNode *first = parse_single_declarator(p, spec.typeName, t->line, id);
                if (spec.isStatic) first->is_static = 1;
                if (!p_check(p, TOK_COMMA)) {
                    p_expect(p, TOK_SEMICOLON, "';' after declaration");
                    node_add_child(root, first);
                } else {
                    NovaNode *group = node_new(NODE_DECL_LIST, t->line);
                    node_add_child(group, first);
                    while (p_match(p, TOK_COMMA)) {
                        NovaNode *d = parse_single_declarator(p, spec.typeName, t->line, NULL);
                        if (spec.isStatic) d->is_static = 1;
                        node_add_child(group, d);
                    }
                    p_expect(p, TOK_SEMICOLON, "';' after declaration");
                    node_add_child(root, group);
                }
            }
            continue;
        }

        if (t->type == TOK_IDENTIFIER &&
            (p_at(p, 1)->type == TOK_IDENTIFIER || p_at(p, 1)->type == TOK_STAR)) {
            diag_add(diags, "error", t->line, t->column,
                "Unknown type '%s' — typedef names are not supported in the NOVA C subset", t->lexeme);
            skip_declaration(p);
            continue;
        }

        diag_add(diags, "error", t->line, t->column, "Expected declaration but found '%s'", t->lexeme);
        p_advance(p);
    }

    return root;
}

const char *nova_node_type_name(NovaNodeType type) {
    switch (type) {
        case NODE_PROGRAM: return "NODE_PROGRAM";
        case NODE_FUNCTION_DEF: return "NODE_FUNCTION_DEF";
        case NODE_PARAMETER: return "NODE_PARAMETER";
        case NODE_STRUCT_DEF: return "NODE_STRUCT_DEF";
        case NODE_STRUCT_FIELD: return "NODE_STRUCT_FIELD";
        case NODE_VAR_DECL: return "NODE_VAR_DECL";
        case NODE_DECL_LIST: return "NODE_DECL_LIST";
        case NODE_IF_STMT: return "NODE_IF_STMT";
        case NODE_WHILE_STMT: return "NODE_WHILE_STMT";
        case NODE_DO_WHILE_STMT: return "NODE_DO_WHILE_STMT";
        case NODE_SWITCH_STMT: return "NODE_SWITCH_STMT";
        case NODE_CASE: return "NODE_CASE";
        case NODE_DEFAULT: return "NODE_DEFAULT";
        case NODE_GOTO: return "NODE_GOTO";
        case NODE_LABEL_STMT: return "NODE_LABEL_STMT";
        case NODE_FOR_STMT: return "NODE_FOR_STMT";
        case NODE_RETURN_STMT: return "NODE_RETURN_STMT";
        case NODE_BREAK_STMT: return "NODE_BREAK_STMT";
        case NODE_CONTINUE_STMT: return "NODE_CONTINUE_STMT";
        case NODE_COMPOUND_STMT: return "NODE_COMPOUND_STMT";
        case NODE_EXPRESSION_STMT: return "NODE_EXPRESSION_STMT";
        case NODE_BINARY_OP: return "NODE_BINARY_OP";
        case NODE_UNARY_OP: return "NODE_UNARY_OP";
        case NODE_TERNARY: return "NODE_TERNARY";
        case NODE_CAST: return "NODE_CAST";
        case NODE_SIZEOF: return "NODE_SIZEOF";
        case NODE_ASSIGNMENT: return "NODE_ASSIGNMENT";
        case NODE_COMPOUND_ASSIGN: return "NODE_COMPOUND_ASSIGN";
        case NODE_FUNC_CALL: return "NODE_FUNC_CALL";
        case NODE_INDEX: return "NODE_INDEX";
        case NODE_MEMBER: return "NODE_MEMBER";
        case NODE_INT_LITERAL: return "NODE_INT_LITERAL";
        case NODE_FLOAT_LITERAL: return "NODE_FLOAT_LITERAL";
        case NODE_STRING_LITERAL: return "NODE_STRING_LITERAL";
        case NODE_IDENTIFIER: return "NODE_IDENTIFIER";
        case NODE_EMPTY: return "NODE_EMPTY";
        case NODE_ERROR: return "NODE_ERROR";
        default: return "NODE_ERROR";
    }
}

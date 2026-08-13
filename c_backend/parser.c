#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* AST helpers                                                                */
/* ------------------------------------------------------------------------- */

ASTNode* node_new(const char* node_type, int line) {
    ASTNode* n = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (!n) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    n->node_type = node_type;
    n->line = line;
    n->child_capacity = 4;
    n->children = (ASTNode**)malloc(sizeof(ASTNode*) * (size_t)n->child_capacity);
    if (!n->children) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return n;
}

void node_add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        parent->child_capacity *= 2;
        ASTNode** grown = (ASTNode**)realloc(parent->children, sizeof(ASTNode*) * (size_t)parent->child_capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        parent->children = grown;
    }
    parent->children[parent->child_count++] = child;
}

void ast_free(ASTNode* node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) ast_free(node->children[i]);
    free(node->children);
    free(node);
}

/* ------------------------------------------------------------------------- */
/* Parser                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    TokenList* tokens;
    int pos;
    DiagList* diags;
    StrPool* pool;
} Parser;

static Token* p_peek(Parser* p) { return &p->tokens->items[p->pos]; }
static int p_check(Parser* p, TokenType t) { return p_peek(p)->type == t; }
static Token* p_advance(Parser* p) {
    Token* t = p_peek(p);
    if (p->pos < p->tokens->count - 1) p->pos++;
    return t;
}
static int p_match(Parser* p, TokenType t) {
    if (p_check(p, t)) { p_advance(p); return 1; }
    return 0;
}
static Token* p_expect(Parser* p, TokenType t, const char* what) {
    if (p_check(p, t)) return p_advance(p);
    Token* tok = p_peek(p);
    diag_add(p->diags, "error", tok->line, tok->column, "Expected %s but found '%s'", what, tok->lexeme);
    return NULL;
}
static void p_skip_to_next_line(Parser* p) {
    int line = p_peek(p)->line;
    while (!p_check(p, TOKEN_EOF) && p_peek(p)->line == line) p_advance(p);
}

static int is_type_token(Parser* p) {
    TokenType t = p_peek(p)->type;
    return t == TOKEN_INT || t == TOKEN_FLOAT || t == TOKEN_DOUBLE || t == TOKEN_CHAR || t == TOKEN_VOID;
}

static const char* type_name_of(TokenType t) {
    switch (t) {
        case TOKEN_INT: return "int";
        case TOKEN_FLOAT: return "float";
        case TOKEN_DOUBLE: return "double";
        case TOKEN_CHAR: return "char";
        case TOKEN_VOID: return "void";
        default: return "int";
    }
}

/* Forward declarations */
static ASTNode* parse_expression(Parser* p);
static ASTNode* parse_statement(Parser* p, int in_loop);

/* ---- expressions ---- */

static ASTNode* parse_assignment(Parser* p);

static ASTNode* binary_level(Parser* p, ASTNode* (*next)(Parser*), const TokenType* ops, const char** opstrs, int nops) {
    ASTNode* left = next(p);
    for (;;) {
        int found = -1;
        for (int i = 0; i < nops; i++) {
            if (p_peek(p)->type == ops[i]) { found = i; break; }
        }
        if (found < 0) break;
        Token* t = p_advance(p);
        ASTNode* node = node_new("NODE_BINARY_OP", t->line);
        strcpy(node->op, opstrs[found]);
        node->has_op = 1;
        node_add_child(node, left);
        node_add_child(node, next(p));
        left = node;
    }
    return left;
}

static ASTNode* parse_primary(Parser* p);
static ASTNode* parse_unary(Parser* p);
static ASTNode* parse_postfix(Parser* p);

static ASTNode* parse_multiplicative(Parser* p) {
    static const TokenType ops[] = { TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT };
    static const char* opstrs[] = { "*", "/", "%" };
    return binary_level(p, parse_unary, ops, opstrs, 3);
}
static ASTNode* parse_additive(Parser* p) {
    static const TokenType ops[] = { TOKEN_PLUS, TOKEN_MINUS };
    static const char* opstrs[] = { "+", "-" };
    return binary_level(p, parse_multiplicative, ops, opstrs, 2);
}
static ASTNode* parse_relational(Parser* p) {
    static const TokenType ops[] = { TOKEN_LT, TOKEN_GT, TOKEN_LEQ, TOKEN_GEQ };
    static const char* opstrs[] = { "<", ">", "<=", ">=" };
    return binary_level(p, parse_additive, ops, opstrs, 4);
}
static ASTNode* parse_equality(Parser* p) {
    static const TokenType ops[] = { TOKEN_EQ, TOKEN_NEQ };
    static const char* opstrs[] = { "==", "!=" };
    return binary_level(p, parse_relational, ops, opstrs, 2);
}
static ASTNode* parse_logical_and(Parser* p) {
    static const TokenType ops[] = { TOKEN_AND };
    static const char* opstrs[] = { "&&" };
    return binary_level(p, parse_equality, ops, opstrs, 1);
}
static ASTNode* parse_logical_or(Parser* p) {
    static const TokenType ops[] = { TOKEN_OR };
    static const char* opstrs[] = { "||" };
    return binary_level(p, parse_logical_and, ops, opstrs, 1);
}

static ASTNode* parse_assignment(Parser* p) {
    ASTNode* left = parse_logical_or(p);
    Token* t = p_peek(p);
    const char* opstr = NULL;
    if (t->type == TOKEN_ASSIGN) opstr = "=";
    else if (t->type == TOKEN_PLUS_ASSIGN) opstr = "+=";
    else if (t->type == TOKEN_MINUS_ASSIGN) opstr = "-=";
    else if (t->type == TOKEN_STAR_ASSIGN) opstr = "*=";
    else if (t->type == TOKEN_SLASH_ASSIGN) opstr = "/=";
    else if (t->type == TOKEN_PERCENT_ASSIGN) opstr = "%=";
    if (opstr) {
        p_advance(p);
        ASTNode* right = parse_assignment(p); /* right-associative */
        ASTNode* node = node_new(strcmp(opstr, "=") == 0 ? "NODE_ASSIGNMENT" : "NODE_COMPOUND_ASSIGN", t->line);
        strcpy(node->op, opstr);
        node->has_op = 1;
        node_add_child(node, left);
        node_add_child(node, right);
        return node;
    }
    return left;
}

static ASTNode* parse_expression(Parser* p) { return parse_assignment(p); }

static ASTNode* parse_unary(Parser* p) {
    Token* t = p_peek(p);
    const char* opstr = NULL;
    if (t->type == TOKEN_MINUS) opstr = "-";
    else if (t->type == TOKEN_NOT) opstr = "!";
    else if (t->type == TOKEN_STAR) opstr = "*";
    else if (t->type == TOKEN_AMPERSAND) opstr = "&";
    else if (t->type == TOKEN_PLUS_PLUS) opstr = "++";
    else if (t->type == TOKEN_MINUS_MINUS) opstr = "--";
    if (opstr) {
        p_advance(p);
        ASTNode* node = node_new("NODE_UNARY_OP", t->line);
        strcpy(node->op, opstr);
        node->has_op = 1;
        node_add_child(node, parse_unary(p));
        return node;
    }
    return parse_postfix(p);
}

static ASTNode* parse_postfix(Parser* p) {
    ASTNode* expr = parse_primary(p);
    for (;;) {
        Token* t = p_peek(p);
        if (t->type == TOKEN_LPAREN) {
            p_advance(p);
            ASTNode* call = node_new("NODE_FUNC_CALL", t->line);
            if (expr->node_type && strcmp(expr->node_type, "NODE_IDENTIFIER") == 0) {
                strcpy(call->identifier, expr->identifier);
                call->has_identifier = 1;
            } else {
                strcpy(call->identifier, "<expr>");
                call->has_identifier = 1;
            }
            if (!p_check(p, TOKEN_RPAREN)) {
                for (;;) {
                    node_add_child(call, parse_expression(p));
                    if (p_match(p, TOKEN_COMMA)) continue;
                    break;
                }
            }
            p_expect(p, TOKEN_RPAREN, "')' after arguments");
            ast_free(expr);
            expr = call;
        } else if (t->type == TOKEN_LBRACKET) {
            p_advance(p);
            ASTNode* idx = node_new("NODE_INDEX", t->line);
            node_add_child(idx, expr);
            node_add_child(idx, parse_expression(p));
            p_expect(p, TOKEN_RBRACKET, "']' after index");
            expr = idx;
        } else if (t->type == TOKEN_DOT || t->type == TOKEN_ARROW) {
            p_advance(p);
            ASTNode* mem = node_new("NODE_MEMBER", t->line);
            node_add_child(mem, expr);
            Token* id = p_expect(p, TOKEN_IDENTIFIER, "field name after \".\"");
            if (id) { strcpy(mem->identifier, id->lexeme); mem->has_identifier = 1; }
            else { strcpy(mem->identifier, "<error>"); mem->has_identifier = 1; }
            expr = mem;
        } else if (t->type == TOKEN_PLUS_PLUS || t->type == TOKEN_MINUS_MINUS) {
            p_advance(p);
            ASTNode* node = node_new("NODE_UNARY_OP", t->line);
            strcpy(node->op, t->type == TOKEN_PLUS_PLUS ? "p++" : "p--");
            node->has_op = 1;
            node_add_child(node, expr);
            expr = node;
        } else {
            break;
        }
    }
    return expr;
}

static long long parse_int_literal(const char* lexeme) {
    return strtoll(lexeme, NULL, 0);
}

static ASTNode* parse_primary(Parser* p) {
    Token* t = p_peek(p);
    if (t->type == TOKEN_INTEGER_LITERAL) {
        p_advance(p);
        ASTNode* node = node_new("NODE_INT_LITERAL", t->line);
        node->num_val = (double)parse_int_literal(t->lexeme);
        node->has_num = 1;
        return node;
    }
    if (t->type == TOKEN_FLOAT_LITERAL) {
        p_advance(p);
        ASTNode* node = node_new("NODE_FLOAT_LITERAL", t->line);
        node->num_val = strtod(t->lexeme, NULL);
        node->has_num = 1;
        return node;
    }
    if (t->type == TOKEN_CHAR_LITERAL) {
        p_advance(p);
        ASTNode* node = node_new("NODE_INT_LITERAL", t->line);
        node->num_val = (double)t->char_value;
        node->has_num = 1;
        return node;
    }
    if (t->type == TOKEN_STRING_LITERAL) {
        p_advance(p);
        ASTNode* node = node_new("NODE_STRING_LITERAL", t->line);
        node->string_val = t->string_value;
        node->has_string = 1;
        return node;
    }
    if (t->type == TOKEN_IDENTIFIER) {
        p_advance(p);
        ASTNode* node = node_new("NODE_IDENTIFIER", t->line);
        strcpy(node->identifier, t->lexeme);
        node->has_identifier = 1;
        return node;
    }
    if (t->type == TOKEN_LPAREN) {
        p_advance(p);
        ASTNode* inner = parse_expression(p);
        p_expect(p, TOKEN_RPAREN, "')'");
        return inner;
    }
    diag_add(p->diags, "error", t->line, t->column, "Unexpected token '%s' in expression", t->lexeme);
    p_advance(p);
    return node_new("NODE_ERROR", t->line);
}

/* ---- declarations & statements ---- */

static ASTNode* int_literal_node(Parser* p, Token* tok) {
    (void)p;
    ASTNode* nn = node_new("NODE_INT_LITERAL", tok->line);
    nn->num_val = (double)parse_int_literal(tok->lexeme);
    nn->has_num = 1;
    return nn;
}

static ASTNode* parse_single_declarator(Parser* p, const char* type_name, int line, Token* first_id) {
    ASTNode* decl = node_new("NODE_VAR_DECL", line);
    char type[64];
    snprintf(type, sizeof(type) - 7, "%s", type_name);
    type[sizeof(type) - 8] = '\0';
    if (!first_id) {
        while (p_check(p, TOKEN_STAR)) { p_advance(p); strcat(type, "*"); }
    }
    Token* id = first_id ? first_id : p_expect(p, TOKEN_IDENTIFIER, "variable name");
    if (id) { strcpy(decl->identifier, id->lexeme); decl->has_identifier = 1; }
    else { strcpy(decl->identifier, "<error>"); decl->has_identifier = 1; }
    strcpy(decl->type_name, type);
    decl->has_type_name = 1;
    if (p_match(p, TOKEN_LBRACKET)) {
        decl->is_array = 1;
        if (p_check(p, TOKEN_INTEGER_LITERAL)) {
            Token* sz = p_advance(p);
            node_add_child(decl, int_literal_node(p, sz));
            decl->has_size = 1;
        }
        p_expect(p, TOKEN_RBRACKET, "']' after array size");
    }
    if (p_match(p, TOKEN_ASSIGN)) {
        if (decl->is_array && p_check(p, TOKEN_LBRACE)) {
            p_advance(p);
            while (!p_check(p, TOKEN_RBRACE) && !p_check(p, TOKEN_EOF)) {
                node_add_child(decl, parse_expression(p));
                if (p_match(p, TOKEN_COMMA)) continue;
                break;
            }
            p_expect(p, TOKEN_RBRACE, "'}' after initializer list");
        } else {
            node_add_child(decl, parse_expression(p));
        }
    }
    return decl;
}

static ASTNode* parse_var_decl_tail(Parser* p, const char* type_name, int line) {
    ASTNode* first = parse_single_declarator(p, type_name, line, NULL);
    if (!p_check(p, TOKEN_COMMA)) {
        p_expect(p, TOKEN_SEMICOLON, "';' after declaration");
        return first;
    }
    ASTNode* group = node_new("NODE_DECL_LIST", line);
    node_add_child(group, first);
    while (p_match(p, TOKEN_COMMA)) {
        node_add_child(group, parse_single_declarator(p, type_name, line, NULL));
    }
    p_expect(p, TOKEN_SEMICOLON, "';' after declaration");
    return group;
}

static ASTNode* parse_statement(Parser* p, int in_loop) {
    Token* t = p_peek(p);

    if (t->type == TOKEN_IF) {
        p_advance(p);
        ASTNode* node = node_new("NODE_IF_STMT", t->line);
        p_expect(p, TOKEN_LPAREN, "'(' after 'if'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOKEN_RPAREN, "')' after condition");
        node_add_child(node, parse_statement(p, in_loop));
        if (p_match(p, TOKEN_ELSE)) node_add_child(node, parse_statement(p, in_loop));
        return node;
    }
    if (t->type == TOKEN_WHILE) {
        p_advance(p);
        ASTNode* node = node_new("NODE_WHILE_STMT", t->line);
        p_expect(p, TOKEN_LPAREN, "'(' after 'while'");
        node_add_child(node, parse_expression(p));
        p_expect(p, TOKEN_RPAREN, "')' after condition");
        node_add_child(node, parse_statement(p, 1));
        return node;
    }
    if (t->type == TOKEN_FOR) {
        p_advance(p);
        ASTNode* node = node_new("NODE_FOR_STMT", t->line);
        p_expect(p, TOKEN_LPAREN, "'(' after 'for'");
        if (p_check(p, TOKEN_SEMICOLON)) {
            p_advance(p);
            node_add_child(node, node_new("NODE_EMPTY", t->line));
        } else if (is_type_token(p)) {
            Token* type_tok = p_advance(p);
            node_add_child(node, parse_var_decl_tail(p, type_name_of(type_tok->type), type_tok->line));
        } else {
            ASTNode* e = node_new("NODE_EXPRESSION_STMT", t->line);
            node_add_child(e, parse_expression(p));
            p_expect(p, TOKEN_SEMICOLON, "';' after for-loop initializer");
            node_add_child(node, e);
        }
        if (p_check(p, TOKEN_SEMICOLON)) {
            node_add_child(node, node_new("NODE_EMPTY", t->line));
        } else {
            node_add_child(node, parse_expression(p));
        }
        p_expect(p, TOKEN_SEMICOLON, "';' after for-loop condition");
        if (p_check(p, TOKEN_RPAREN)) {
            node_add_child(node, node_new("NODE_EMPTY", t->line));
        } else {
            node_add_child(node, parse_expression(p));
        }
        p_expect(p, TOKEN_RPAREN, "')' after for-loop clauses");
        node_add_child(node, parse_statement(p, 1));
        return node;
    }
    if (t->type == TOKEN_RETURN) {
        p_advance(p);
        ASTNode* node = node_new("NODE_RETURN_STMT", t->line);
        if (!p_check(p, TOKEN_SEMICOLON)) node_add_child(node, parse_expression(p));
        p_expect(p, TOKEN_SEMICOLON, "';' after return");
        return node;
    }
    if (t->type == TOKEN_BREAK) {
        p_advance(p);
        p_expect(p, TOKEN_SEMICOLON, "';' after break");
        return node_new("NODE_BREAK_STMT", t->line);
    }
    if (t->type == TOKEN_CONTINUE) {
        p_advance(p);
        p_expect(p, TOKEN_SEMICOLON, "';' after continue");
        return node_new("NODE_CONTINUE_STMT", t->line);
    }
    if (t->type == TOKEN_LBRACE) {
        p_advance(p);
        ASTNode* comp = node_new("NODE_COMPOUND_STMT", t->line);
        while (!p_check(p, TOKEN_RBRACE) && !p_check(p, TOKEN_EOF)) {
            node_add_child(comp, parse_statement(p, in_loop));
        }
        p_expect(p, TOKEN_RBRACE, "'}' to close block");
        return comp;
    }
    if (is_type_token(p)) {
        Token* type_tok = p_advance(p);
        return parse_var_decl_tail(p, type_name_of(type_tok->type), type_tok->line);
    }
    if (t->type == TOKEN_STRUCT) {
        p_advance(p);
        Token* name_tok = p_expect(p, TOKEN_IDENTIFIER, "struct name");
        char type_name[80];
        snprintf(type_name, sizeof(type_name), "struct %s", name_tok ? name_tok->lexeme : "<error>");
        return parse_var_decl_tail(p, type_name, t->line);
    }
    if (t->type == TOKEN_SEMICOLON) {
        p_advance(p);
        return node_new("NODE_EMPTY", t->line);
    }

    /* expression statement */
    int before = p->pos;
    ASTNode* expr = parse_expression(p);
    if (p->pos == before) {
        diag_add(p->diags, "error", t->line, t->column, "Unexpected token '%s'", t->lexeme);
        p_advance(p);
        ast_free(expr);
        return node_new("NODE_ERROR", t->line);
    }
    ASTNode* stmt = node_new("NODE_EXPRESSION_STMT", t->line);
    node_add_child(stmt, expr);
    p_expect(p, TOKEN_SEMICOLON, "';' after expression");
    return stmt;
}

static void parse_parameter_list(Parser* p, ASTNode* func) {
    if (p_check(p, TOKEN_RPAREN)) return;
    if (p_check(p, TOKEN_VOID) && p->pos + 1 < p->tokens->count &&
        p->tokens->items[p->pos + 1].type == TOKEN_RPAREN) {
        p_advance(p); /* int main(void) */
        return;
    }
    for (;;) {
        if (!is_type_token(p)) {
            Token* tok = p_peek(p);
            diag_add(p->diags, "error", tok->line, tok->column,
                     "Expected parameter type but found '%s'", tok->lexeme);
            break;
        }
        Token* type_tok = p_advance(p);
        char type_name[64];
        snprintf(type_name, sizeof(type_name) - 7, "%s", type_name_of(type_tok->type));
        type_name[sizeof(type_name) - 8] = '\0';
        int is_pointer = 0;
        while (p_check(p, TOKEN_STAR)) { p_advance(p); is_pointer = 1; }
        Token* id = p_expect(p, TOKEN_IDENTIFIER, "parameter name");
        ASTNode* param = node_new("NODE_PARAMETER", type_tok->line);
        if (is_pointer) strcat(type_name, "*");
        strcpy(param->type_name, type_name);
        param->has_type_name = 1;
        if (id) { strcpy(param->identifier, id->lexeme); param->has_identifier = 1; }
        else { strcpy(param->identifier, "<error>"); param->has_identifier = 1; }
        node_add_child(func, param);
        if (p_match(p, TOKEN_COMMA)) continue;
        break;
    }
}

ASTNode* parse_program(TokenList* tokens, DiagList* diags, StrPool* pool) {
    Parser parser;
    parser.tokens = tokens;
    parser.pos = 0;
    parser.diags = diags;
    parser.pool = pool;
    Parser* p = &parser;

    ASTNode* root = node_new("NODE_PROGRAM", 1);

    while (!p_check(p, TOKEN_EOF)) {
        Token* t = p_peek(p);

        if (t->type == TOKEN_INCLUDE || t->type == TOKEN_DEFINE || t->type == TOKEN_HASH) {
            p_skip_to_next_line(p);
            continue;
        }

        if (t->type == TOKEN_STRUCT) {
            p_advance(p);
            Token* name_tok = p_expect(p, TOKEN_IDENTIFIER, "struct name");
            if (p_check(p, TOKEN_LBRACE)) {
                p_advance(p);
                ASTNode* def = node_new("NODE_STRUCT_DEF", t->line);
                if (name_tok) { strcpy(def->identifier, name_tok->lexeme); def->has_identifier = 1; }
                else { strcpy(def->identifier, "<error>"); def->has_identifier = 1; }
                while (!p_check(p, TOKEN_RBRACE) && !p_check(p, TOKEN_EOF)) {
                    if (!is_type_token(p)) {
                        Token* tok = p_peek(p);
                        diag_add(p->diags, "error", tok->line, tok->column,
                                 "Expected field type in struct but found '%s'", tok->lexeme);
                        while (!p_check(p, TOKEN_EOF) && !p_check(p, TOKEN_SEMICOLON) && !p_check(p, TOKEN_RBRACE))
                            p_advance(p);
                        p_match(p, TOKEN_SEMICOLON);
                        continue;
                    }
                    Token* ft = p_advance(p);
                    ASTNode* field = node_new("NODE_STRUCT_FIELD", ft->line);
                    strcpy(field->type_name, type_name_of(ft->type));
                    field->has_type_name = 1;
                    Token* fid = p_expect(p, TOKEN_IDENTIFIER, "field name");
                    if (fid) { strcpy(field->identifier, fid->lexeme); field->has_identifier = 1; }
                    else { strcpy(field->identifier, "<error>"); field->has_identifier = 1; }
                    if (p_match(p, TOKEN_LBRACKET)) {
                        field->is_array = 1;
                        if (p_check(p, TOKEN_INTEGER_LITERAL)) {
                            Token* sz = p_advance(p);
                            node_add_child(field, int_literal_node(p, sz));
                            field->has_size = 1;
                        }
                        p_expect(p, TOKEN_RBRACKET, "']' after field size");
                    }
                    p_expect(p, TOKEN_SEMICOLON, "';' after struct field");
                    node_add_child(def, field);
                }
                p_expect(p, TOKEN_RBRACE, "'}' to close struct");
                p_expect(p, TOKEN_SEMICOLON, "';' after struct definition");
                node_add_child(root, def);
            } else {
                char type_name[80];
                snprintf(type_name, sizeof(type_name), "struct %s",
                         name_tok ? name_tok->lexeme : "<error>");
                node_add_child(root, parse_var_decl_tail(p, type_name, t->line));
            }
            continue;
        }

        if (is_type_token(p)) {
            Token* type_tok = p_advance(p);
            char type_name[64];
            snprintf(type_name, sizeof(type_name) - 15, "%s", type_name_of(type_tok->type));
            type_name[sizeof(type_name) - 16] = '\0';
            while (p_check(p, TOKEN_STAR)) { p_advance(p); strcat(type_name, "*"); }
            Token* id = p_expect(p, TOKEN_IDENTIFIER, "function or variable name");
            if (!id) {
                while (!p_check(p, TOKEN_EOF) && !p_check(p, TOKEN_SEMICOLON) && !p_check(p, TOKEN_RBRACE))
                    p_advance(p);
                p_match(p, TOKEN_SEMICOLON);
                continue;
            }
            if (p_check(p, TOKEN_LPAREN)) {
                p_advance(p);
                ASTNode* func = node_new("NODE_FUNCTION_DEF", type_tok->line);
                strcpy(func->type_name, type_name);
                func->has_type_name = 1;
                strcpy(func->identifier, id->lexeme);
                func->has_identifier = 1;
                parse_parameter_list(p, func);
                p_expect(p, TOKEN_RPAREN, "')' after parameters");
                if (!p_check(p, TOKEN_LBRACE)) {
                    Token* tok = p_peek(p);
                    diag_add(p->diags, "error", tok->line, tok->column,
                             "Expected '{' after function signature");
                    while (!p_check(p, TOKEN_EOF) && !p_check(p, TOKEN_SEMICOLON) && !p_check(p, TOKEN_RBRACE))
                        p_advance(p);
                    p_match(p, TOKEN_SEMICOLON);
                    node_add_child(root, func);
                    continue;
                }
                node_add_child(func, parse_statement(p, 0)); /* body */
                node_add_child(root, func);
            } else {
                ASTNode* first = parse_single_declarator(p, type_name, type_tok->line, id);
                if (!p_check(p, TOKEN_COMMA)) {
                    p_expect(p, TOKEN_SEMICOLON, "';' after declaration");
                    node_add_child(root, first);
                } else {
                    ASTNode* group = node_new("NODE_DECL_LIST", type_tok->line);
                    node_add_child(group, first);
                    while (p_match(p, TOKEN_COMMA)) {
                        node_add_child(group, parse_single_declarator(p, type_name, type_tok->line, NULL));
                    }
                    p_expect(p, TOKEN_SEMICOLON, "';' after declaration");
                    node_add_child(root, group);
                }
            }
            continue;
        }

        diag_add(p->diags, "error", t->line, t->column, "Expected declaration but found '%s'", t->lexeme);
        p_advance(p);
    }

    return root;
}

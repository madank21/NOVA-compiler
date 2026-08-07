#include "parser.h"

Parser* parser_init(TokenList* tokens) {
    Parser* parser = (Parser*)malloc(sizeof(Parser));
    parser->tokens = tokens;
    parser->current = 0;
    return parser;
}

void parser_free(Parser* parser) {
    if (parser) free(parser);
}

static Token peek_token(Parser* parser) {
    if (parser->current >= parser->tokens->count) {
        return parser->tokens->tokens[parser->tokens->count - 1];
    }
    return parser->tokens->tokens[parser->current];
}

static Token advance_token(Parser* parser) {
    Token tok = peek_token(parser);
    if (parser->current < parser->tokens->count) {
        parser->current++;
    }
    return tok;
}

static int match_token(Parser* parser, TokenType type) {
    if (peek_token(parser).type == type) {
        advance_token(parser);
        return 1;
    }
    return 0;
}

ASTNode* create_ast_node(ASTNodeType type) {
    ASTNode* node = (ASTNode*)calloc(1, sizeof(ASTNode));
    node->type = type;
    node->child_capacity = 4;
    node->child_count = 0;
    node->children = (ASTNode**)malloc(sizeof(ASTNode*) * node->child_capacity);
    return node;
}

void ast_add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        parent->child_capacity *= 2;
        parent->children = (ASTNode**)realloc(parent->children, sizeof(ASTNode*) * parent->child_capacity);
    }
    parent->children[parent->child_count++] = child;
}

void ast_free(ASTNode* node) {
    if (!node) return;
    if (node->left) ast_free(node->left);
    if (node->right) ast_free(node->right);
    if (node->condition) ast_free(node->condition);
    if (node->else_branch) ast_free(node->else_branch);
    if (node->init) ast_free(node->init);
    if (node->increment) ast_free(node->increment);
    for (int i = 0; i < node->child_count; i++) {
        ast_free(node->children[i]);
    }
    if (node->children) free(node->children);
    free(node);
}

static ASTNode* parse_expression(Parser* parser);
static ASTNode* parse_statement(Parser* parser);

static ASTNode* parse_primary(Parser* parser) {
    Token tok = peek_token(parser);
    if (tok.type == TOKEN_INTEGER_LITERAL) {
        advance_token(parser);
        ASTNode* node = create_ast_node(NODE_INT_LITERAL);
        node->int_val = tok.value.int_value;
        node->line = tok.line;
        return node;
    }
    if (tok.type == TOKEN_FLOAT_LITERAL) {
        advance_token(parser);
        ASTNode* node = create_ast_node(NODE_FLOAT_LITERAL);
        node->float_val = tok.value.float_value;
        node->line = tok.line;
        return node;
    }
    if (tok.type == TOKEN_STRING_LITERAL) {
        advance_token(parser);
        ASTNode* node = create_ast_node(NODE_STRING_LITERAL);
        strncpy(node->string_val, tok.value.string_value, 511);
        node->line = tok.line;
        return node;
    }
    if (tok.type == TOKEN_IDENTIFIER) {
        advance_token(parser);
        ASTNode* node = create_ast_node(NODE_IDENTIFIER);
        strncpy(node->identifier, tok.lexeme, 255);
        node->line = tok.line;

        // Function call
        if (peek_token(parser).type == TOKEN_LPAREN) {
            advance_token(parser); // (
            ASTNode* call_node = create_ast_node(NODE_FUNC_CALL);
            strncpy(call_node->identifier, tok.lexeme, 255);
            call_node->line = tok.line;

            if (peek_token(parser).type != TOKEN_RPAREN) {
                while (1) {
                    ast_add_child(call_node, parse_expression(parser));
                    if (match_token(parser, TOKEN_COMMA)) continue;
                    break;
                }
            }
            match_token(parser, TOKEN_RPAREN);
            free(node->children);
            free(node);
            return call_node;
        }

        return node;
    }
    if (match_token(parser, TOKEN_LPAREN)) {
        ASTNode* expr = parse_expression(parser);
        match_token(parser, TOKEN_RPAREN);
        return expr;
    }
    advance_token(parser);
    return create_ast_node(NODE_INT_LITERAL);
}

static ASTNode* parse_multiplicative(Parser* parser) {
    ASTNode* left = parse_primary(parser);
    while (peek_token(parser).type == TOKEN_STAR || peek_token(parser).type == TOKEN_SLASH || peek_token(parser).type == TOKEN_PERCENT) {
        Token op_tok = advance_token(parser);
        ASTNode* bin = create_ast_node(NODE_BINARY_OP);
        strncpy(bin->op, op_tok.lexeme, 15);
        bin->left = left;
        bin->right = parse_primary(parser);
        left = bin;
    }
    return left;
}

static ASTNode* parse_additive(Parser* parser) {
    ASTNode* left = parse_multiplicative(parser);
    while (peek_token(parser).type == TOKEN_PLUS || peek_token(parser).type == TOKEN_MINUS) {
        Token op_tok = advance_token(parser);
        ASTNode* bin = create_ast_node(NODE_BINARY_OP);
        strncpy(bin->op, op_tok.lexeme, 15);
        bin->left = left;
        bin->right = parse_multiplicative(parser);
        left = bin;
    }
    return left;
}

static ASTNode* parse_relational(Parser* parser) {
    ASTNode* left = parse_additive(parser);
    while (peek_token(parser).type == TOKEN_LT || peek_token(parser).type == TOKEN_GT ||
           peek_token(parser).type == TOKEN_LEQ || peek_token(parser).type == TOKEN_GEQ ||
           peek_token(parser).type == TOKEN_EQ || peek_token(parser).type == TOKEN_NEQ) {
        Token op_tok = advance_token(parser);
        ASTNode* bin = create_ast_node(NODE_BINARY_OP);
        strncpy(bin->op, op_tok.lexeme, 15);
        bin->left = left;
        bin->right = parse_additive(parser);
        left = bin;
    }
    return left;
}

static ASTNode* parse_expression(Parser* parser) {
    ASTNode* left = parse_relational(parser);
    if (peek_token(parser).type == TOKEN_ASSIGN) {
        advance_token(parser);
        ASTNode* assign = create_ast_node(NODE_ASSIGNMENT);
        assign->left = left;
        assign->right = parse_expression(parser);
        return assign;
    }
    return left;
}

static ASTNode* parse_compound_stmt(Parser* parser) {
    match_token(parser, TOKEN_LBRACE);
    ASTNode* compound = create_ast_node(NODE_COMPOUND_STMT);
    while (peek_token(parser).type != TOKEN_RBRACE && peek_token(parser).type != TOKEN_EOF) {
        ast_add_child(compound, parse_statement(parser));
    }
    match_token(parser, TOKEN_RBRACE);
    return compound;
}

static ASTNode* parse_statement(Parser* parser) {
    Token tok = peek_token(parser);
    if (tok.type == TOKEN_IF) {
        advance_token(parser);
        ASTNode* if_node = create_ast_node(NODE_IF_STMT);
        match_token(parser, TOKEN_LPAREN);
        if_node->condition = parse_expression(parser);
        match_token(parser, TOKEN_RPAREN);
        if_node->left = parse_statement(parser);
        if (peek_token(parser).type == TOKEN_ELSE) {
            advance_token(parser);
            if_node->else_branch = parse_statement(parser);
        }
        return if_node;
    }
    if (tok.type == TOKEN_WHILE) {
        advance_token(parser);
        ASTNode* while_node = create_ast_node(NODE_WHILE_STMT);
        match_token(parser, TOKEN_LPAREN);
        while_node->condition = parse_expression(parser);
        match_token(parser, TOKEN_RPAREN);
        while_node->left = parse_statement(parser);
        return while_node;
    }
    if (tok.type == TOKEN_FOR) {
        advance_token(parser);
        ASTNode* for_node = create_ast_node(NODE_FOR_STMT);
        match_token(parser, TOKEN_LPAREN);
        for_node->init = parse_statement(parser);
        for_node->condition = parse_expression(parser);
        match_token(parser, TOKEN_SEMICOLON);
        for_node->increment = parse_expression(parser);
        match_token(parser, TOKEN_RPAREN);
        for_node->left = parse_statement(parser);
        return for_node;
    }
    if (tok.type == TOKEN_RETURN) {
        advance_token(parser);
        ASTNode* ret_node = create_ast_node(NODE_RETURN_STMT);
        if (peek_token(parser).type != TOKEN_SEMICOLON) {
            ret_node->left = parse_expression(parser);
        }
        match_token(parser, TOKEN_SEMICOLON);
        return ret_node;
    }
    if (tok.type == TOKEN_INT || tok.type == TOKEN_FLOAT || tok.type == TOKEN_CHAR || tok.type == TOKEN_VOID) {
        advance_token(parser);
        ASTNode* var_decl = create_ast_node(NODE_VAR_DECL);
        strncpy(var_decl->type_name, tok.lexeme, 63);
        Token id_tok = advance_token(parser);
        strncpy(var_decl->identifier, id_tok.lexeme, 255);
        if (match_token(parser, TOKEN_ASSIGN)) {
            var_decl->left = parse_expression(parser);
        }
        match_token(parser, TOKEN_SEMICOLON);
        return var_decl;
    }
    if (tok.type == TOKEN_LBRACE) {
        return parse_compound_stmt(parser);
    }

    ASTNode* expr_stmt = create_ast_node(NODE_EXPRESSION_STMT);
    expr_stmt->left = parse_expression(parser);
    match_token(parser, TOKEN_SEMICOLON);
    return expr_stmt;
}

ASTNode* parse_program(Parser* parser) {
    ASTNode* root = create_ast_node(NODE_PROGRAM);
    while (peek_token(parser).type != TOKEN_EOF) {
        Token tok = peek_token(parser);
        if (tok.type == TOKEN_INCLUDE || tok.type == TOKEN_DEFINE) {
            advance_token(parser);
            while (peek_token(parser).type != TOKEN_SEMICOLON && peek_token(parser).type != TOKEN_EOF) {
                advance_token(parser);
            }
            match_token(parser, TOKEN_SEMICOLON);
            continue;
        }

        if (tok.type == TOKEN_INT || tok.type == TOKEN_FLOAT || tok.type == TOKEN_CHAR || tok.type == TOKEN_VOID) {
            Token type_tok = advance_token(parser);
            Token id_tok = advance_token(parser);

            if (peek_token(parser).type == TOKEN_LPAREN) {
                // Function definition
                advance_token(parser); // (
                ASTNode* func = create_ast_node(NODE_FUNCTION_DEF);
                strncpy(func->type_name, type_tok.lexeme, 63);
                strncpy(func->identifier, id_tok.lexeme, 255);

                if (peek_token(parser).type != TOKEN_RPAREN) {
                    while (1) {
                        if (peek_token(parser).type == TOKEN_INT || peek_token(parser).type == TOKEN_FLOAT || peek_token(parser).type == TOKEN_CHAR) {
                            Token ptype = advance_token(parser);
                            Token pid = advance_token(parser);
                            ASTNode* param = create_ast_node(NODE_PARAMETER);
                            strncpy(param->type_name, ptype.lexeme, 63);
                            strncpy(param->identifier, pid.lexeme, 255);
                            ast_add_child(func, param);
                        }
                        if (match_token(parser, TOKEN_COMMA)) continue;
                        break;
                    }
                }
                match_token(parser, TOKEN_RPAREN);
                func->right = parse_compound_stmt(parser);
                ast_add_child(root, func);
            } else {
                // Global var declaration
                ASTNode* var_decl = create_ast_node(NODE_VAR_DECL);
                strncpy(var_decl->type_name, type_tok.lexeme, 63);
                strncpy(var_decl->identifier, id_tok.lexeme, 255);
                if (match_token(parser, TOKEN_ASSIGN)) {
                    var_decl->left = parse_expression(parser);
                }
                match_token(parser, TOKEN_SEMICOLON);
                ast_add_child(root, var_decl);
            }
        } else {
            advance_token(parser);
        }
    }
    return root;
}

const char* ast_node_type_to_string(ASTNodeType type) {
    switch (type) {
        case NODE_PROGRAM: return "NODE_PROGRAM";
        case NODE_FUNCTION_DEF: return "NODE_FUNCTION_DEF";
        case NODE_PARAMETER: return "NODE_PARAMETER";
        case NODE_COMPOUND_STMT: return "NODE_COMPOUND_STMT";
        case NODE_VAR_DECL: return "NODE_VAR_DECL";
        case NODE_IF_STMT: return "NODE_IF_STMT";
        case NODE_WHILE_STMT: return "NODE_WHILE_STMT";
        case NODE_FOR_STMT: return "NODE_FOR_STMT";
        case NODE_RETURN_STMT: return "NODE_RETURN_STMT";
        case NODE_EXPRESSION_STMT: return "NODE_EXPRESSION_STMT";
        case NODE_BINARY_OP: return "NODE_BINARY_OP";
        case NODE_ASSIGNMENT: return "NODE_ASSIGNMENT";
        case NODE_FUNC_CALL: return "NODE_FUNC_CALL";
        case NODE_INT_LITERAL: return "NODE_INT_LITERAL";
        case NODE_FLOAT_LITERAL: return "NODE_FLOAT_LITERAL";
        case NODE_STRING_LITERAL: return "NODE_STRING_LITERAL";
        case NODE_IDENTIFIER: return "NODE_IDENTIFIER";
        default: return "NODE_AST";
    }
}

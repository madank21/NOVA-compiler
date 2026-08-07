#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

typedef enum {
    NODE_PROGRAM,
    NODE_FUNCTION_DEF,
    NODE_PARAMETER_LIST,
    NODE_PARAMETER,
    NODE_COMPOUND_STMT,
    NODE_VAR_DECL,
    NODE_STRUCT_DEF,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_RETURN_STMT,
    NODE_EXPRESSION_STMT,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_ASSIGNMENT,
    NODE_FUNC_CALL,
    NODE_ARRAY_ACCESS,
    NODE_INT_LITERAL,
    NODE_FLOAT_LITERAL,
    NODE_STRING_LITERAL,
    NODE_IDENTIFIER
} ASTNodeType;

typedef struct ASTNode {
    ASTNodeType type;
    struct ASTNode* left;
    struct ASTNode* right;
    struct ASTNode* condition;
    struct ASTNode* else_branch;
    struct ASTNode* init;
    struct ASTNode* increment;
    struct ASTNode** children;
    int child_count;
    int child_capacity;

    char identifier[256];
    char type_name[64];
    char op[16];
    long long int_val;
    double float_val;
    char string_val[512];

    int line;
    int column;
} ASTNode;

typedef struct {
    TokenList* tokens;
    int current;
} Parser;

Parser* parser_init(TokenList* tokens);
void parser_free(Parser* parser);
ASTNode* parse_program(Parser* parser);
void ast_free(ASTNode* node);
const char* ast_node_type_to_string(ASTNodeType type);

#endif // PARSER_H

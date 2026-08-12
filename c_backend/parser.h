#ifndef NOVA_PARSER_H
#define NOVA_PARSER_H

#include "lexer.h"

/* Children-only AST, mirrored 1:1 from the browser engine. */

typedef struct ASTNode {
    const char* node_type;            /* "NODE_PROGRAM", ... */
    int line;
    char identifier[256]; int has_identifier;
    char type_name[64];   int has_type_name;
    char op[8];           int has_op;
    double num_val;       int has_num;
    char* string_val;     int has_string;
    int is_array;
    int has_size;
    struct ASTNode** children;
    int child_count;
    int child_capacity;
} ASTNode;

ASTNode* node_new(const char* node_type, int line);
void node_add_child(ASTNode* parent, ASTNode* child);
void ast_free(ASTNode* node);

ASTNode* parse_program(TokenList* tokens, DiagList* diags, StrPool* pool);

#endif
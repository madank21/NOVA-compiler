#ifndef NOVA_PARSER_H
#define NOVA_PARSER_H

#include "lexer.h"

typedef enum {
    NODE_PROGRAM, NODE_FUNCTION_DEF, NODE_PARAMETER, NODE_STRUCT_DEF, NODE_STRUCT_FIELD,
    NODE_VAR_DECL, NODE_DECL_LIST, NODE_IF_STMT, NODE_WHILE_STMT, NODE_DO_WHILE_STMT,
    NODE_SWITCH_STMT, NODE_CASE, NODE_DEFAULT, NODE_GOTO, NODE_LABEL_STMT,
    NODE_FOR_STMT, NODE_RETURN_STMT, NODE_BREAK_STMT, NODE_CONTINUE_STMT,
    NODE_COMPOUND_STMT, NODE_EXPRESSION_STMT, NODE_BINARY_OP, NODE_UNARY_OP,
    NODE_TERNARY, NODE_CAST, NODE_SIZEOF,
    NODE_ASSIGNMENT, NODE_COMPOUND_ASSIGN, NODE_FUNC_CALL, NODE_INDEX, NODE_MEMBER,
    NODE_INT_LITERAL, NODE_FLOAT_LITERAL, NODE_STRING_LITERAL, NODE_IDENTIFIER,
    NODE_EMPTY, NODE_ERROR
} NovaNodeType;

typedef struct NovaNode {
    NovaNodeType type;
    int line;
    char identifier[256]; int has_identifier;
    char type_name[96];  int has_type_name;
    char op[8];          int has_op;
    double num_val;      int has_num;
    char *string_val;    int has_string;
    int is_array;
    int has_size;
    int is_static;
    int is_forward;
    /* Semantic-only storage binding (not serialized in the AST contract). */
    int has_binding;
    int binding_offset;
    int binding_is_global;
    struct NovaNode **children;
    int child_count;
    int child_capacity;
} NovaNode;

typedef struct {
    const char *typeName;  /* normalized, points to static storage */
    int isStatic;
    int isExtern;
    int ptr;
    int hasBase;
    int baseKind;          /* 0 int, 1 float/double, 2 void, 3 struct */
    char structName[128];
    char typeNameBuf[160]; /* owned by parser arena */
} TypeSpec;

NovaNode *nova_parse(NovaTokenList *tokens, DiagList *diags);
void nova_ast_free(NovaNode *node);
const char *nova_node_type_name(NovaNodeType type);

#endif
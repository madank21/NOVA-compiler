#ifndef TAC_H
#define TAC_H

#include "parser.h"

typedef enum {
    TAC_ADD, TAC_SUB, TAC_MUL, TAC_DIV, TAC_MOD,
    TAC_ASSIGN, TAC_LABEL, TAC_GOTO, TAC_IF_FALSE, TAC_IF_TRUE,
    TAC_PARAM, TAC_CALL, TAC_RETURN, TAC_FUNC_BEGIN, TAC_FUNC_END,
    TAC_PRINT, TAC_NOP
} TACOpcode;

typedef struct TACInstr {
    TACOpcode op;
    char result[64];
    char arg1[64];
    char arg2[64];
    int line;
    struct TACInstr* next;
} TACInstr;

typedef struct {
    TACInstr* head;
    TACInstr* tail;
    int temp_count;
    int label_count;
    int count;
} TACList;

TACList* generate_tac(ASTNode* root);
void tac_list_free(TACList* list);
const char* tac_op_to_string(TACOpcode op);

#endif // TAC_H

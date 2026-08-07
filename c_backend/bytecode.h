#ifndef BYTECODE_H
#define BYTECODE_H

#include "tac.h"

typedef enum {
    OP_PUSH,
    OP_POP,
    OP_LOAD,
    OP_STORE,
    OP_ADDR,
    OP_LOAD_PTR,
    OP_STORE_PTR,
    OP_LOAD_ARRAY,
    OP_STORE_ARRAY,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_CMP_EQ,
    OP_CMP_LT,
    OP_JMP,
    OP_JZ,
    OP_CALL,
    OP_RET,
    OP_PRINT,
    OP_HALT
} Opcode;

typedef struct {
    Opcode op;
    int operand;
    char symbol[64];
    int line;
} Instruction;

typedef struct {
    Instruction* code;
    int count;
    int capacity;
} BytecodeChunk;

BytecodeChunk* generate_bytecode(TACList* tac_list);
void bytecode_chunk_free(BytecodeChunk* chunk);
const char* opcode_to_string(Opcode op);

#endif // BYTECODE_H

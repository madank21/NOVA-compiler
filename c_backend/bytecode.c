#include "bytecode.h"
#include <ctype.h>

BytecodeChunk* generate_bytecode(TACList* tac_list) {
    BytecodeChunk* chunk = (BytecodeChunk*)malloc(sizeof(BytecodeChunk));
    chunk->capacity = 64;
    chunk->count = 0;
    chunk->code = (Instruction*)malloc(sizeof(Instruction) * chunk->capacity);

    if (!tac_list) return chunk;

    TACInstr* curr = tac_list->head;
    while (curr) {
        Instruction instr;
        memset(&instr, 0, sizeof(Instruction));
        instr.line = curr->line;

        if (curr->op == TAC_ASSIGN) {
            if (isdigit(curr->arg1[0]) || curr->arg1[0] == '-') {
                instr.op = OP_PUSH;
                instr.operand = atoi(curr->arg1);
                chunk->code[chunk->count++] = instr;

                Instruction store;
                memset(&store, 0, sizeof(Instruction));
                store.op = OP_STORE;
                strncpy(store.symbol, curr->result, 63);
                store.line = curr->line;
                chunk->code[chunk->count++] = store;
            } else {
                instr.op = OP_LOAD;
                strncpy(instr.symbol, curr->arg1, 63);
                chunk->code[chunk->count++] = instr;

                Instruction store;
                memset(&store, 0, sizeof(Instruction));
                store.op = OP_STORE;
                strncpy(store.symbol, curr->result, 63);
                store.line = curr->line;
                chunk->code[chunk->count++] = store;
            }
        } else if (curr->op == TAC_ADD || curr->op == TAC_SUB || curr->op == TAC_MUL || curr->op == TAC_DIV) {
            // Load arg1
            if (isdigit(curr->arg1[0]) || curr->arg1[0] == '-') {
                instr.op = OP_PUSH;
                instr.operand = atoi(curr->arg1);
            } else {
                instr.op = OP_LOAD;
                strncpy(instr.symbol, curr->arg1, 63);
            }
            chunk->code[chunk->count++] = instr;

            // Load arg2
            Instruction instr2;
            memset(&instr2, 0, sizeof(Instruction));
            instr2.line = curr->line;
            if (isdigit(curr->arg2[0]) || curr->arg2[0] == '-') {
                instr2.op = OP_PUSH;
                instr2.operand = atoi(curr->arg2);
            } else {
                instr2.op = OP_LOAD;
                strncpy(instr2.symbol, curr->arg2, 63);
            }
            chunk->code[chunk->count++] = instr2;

            // Opcode
            Instruction op_instr;
            memset(&op_instr, 0, sizeof(Instruction));
            op_instr.line = curr->line;
            if (curr->op == TAC_ADD) op_instr.op = OP_ADD;
            else if (curr->op == TAC_SUB) op_instr.op = OP_SUB;
            else if (curr->op == TAC_MUL) op_instr.op = OP_MUL;
            else if (curr->op == TAC_DIV) op_instr.op = OP_DIV;
            chunk->code[chunk->count++] = op_instr;

            // Store result
            Instruction store;
            memset(&store, 0, sizeof(Instruction));
            store.op = OP_STORE;
            strncpy(store.symbol, curr->result, 63);
            store.line = curr->line;
            chunk->code[chunk->count++] = store;
        } else if (curr->op == TAC_PRINT) {
            instr.op = OP_PRINT;
            chunk->code[chunk->count++] = instr;
        } else if (curr->op == TAC_RETURN) {
            if (curr->result[0] != '\0') {
                if (isdigit(curr->result[0]) || curr->result[0] == '-') {
                    instr.op = OP_PUSH;
                    instr.operand = atoi(curr->result);
                } else {
                    instr.op = OP_LOAD;
                    strncpy(instr.symbol, curr->result, 63);
                }
                chunk->code[chunk->count++] = instr;
            }
            Instruction ret_instr;
            memset(&ret_instr, 0, sizeof(Instruction));
            ret_instr.op = OP_RET;
            ret_instr.line = curr->line;
            chunk->code[chunk->count++] = ret_instr;
        }

        curr = curr->next;
    }

    Instruction halt_instr;
    memset(&halt_instr, 0, sizeof(Instruction));
    halt_instr.op = OP_HALT;
    chunk->code[chunk->count++] = halt_instr;

    return chunk;
}

void bytecode_chunk_free(BytecodeChunk* chunk) {
    if (chunk) {
        if (chunk->code) free(chunk->code);
        free(chunk);
    }
}

const char* opcode_to_string(Opcode op) {
    switch (op) {
        case OP_PUSH: return "PUSH";
        case OP_POP: return "POP";
        case OP_LOAD: return "LOAD";
        case OP_STORE: return "STORE";
        case OP_ADDR: return "ADDR";
        case OP_LOAD_PTR: return "LOAD_PTR";
        case OP_STORE_PTR: return "STORE_PTR";
        case OP_LOAD_ARRAY: return "LOAD_ARRAY";
        case OP_STORE_ARRAY: return "STORE_ARRAY";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_CMP_EQ: return "CMP_EQ";
        case OP_CMP_LT: return "CMP_LT";
        case OP_JMP: return "JMP";
        case OP_JZ: return "JZ";
        case OP_CALL: return "CALL";
        case OP_RET: return "RET";
        case OP_PRINT: return "PRINT";
        case OP_HALT: return "HALT";
        default: return "UNKNOWN";
    }
}

#ifndef VM_H
#define VM_H

#include "bytecode.h"

#define STACK_MAX 256
#define VARS_MAX 64

typedef struct {
    char name[64];
    int value;
} VMVariable;

typedef struct {
    int pc;
    int operand_stack[STACK_MAX];
    int stack_top;

    char call_stack[16][64];
    int call_top;

    VMVariable variables[VARS_MAX];
    int var_count;

    char console_output[2048];
    int line;
} VMStep;

typedef struct {
    VMStep* steps;
    int count;
    int capacity;
} VMExecutionTrace;

typedef struct {
    BytecodeChunk* chunk;
    int pc;
    int stack[STACK_MAX];
    int sp;
    VMVariable vars[VARS_MAX];
    int var_count;
    char console[2048];
} VM;

VMExecutionTrace* vm_execute(BytecodeChunk* chunk);
void vm_trace_free(VMExecutionTrace* trace);

#endif // VM_H

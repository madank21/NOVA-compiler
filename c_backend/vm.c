#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_var(VM* vm, const char* name) {
    for (int i = 0; i < vm->var_count; i++) {
        if (strcmp(vm->vars[i].name, name) == 0) return vm->vars[i].value;
    }
    return 0;
}

static void set_var(VM* vm, const char* name, int value) {
    for (int i = 0; i < vm->var_count; i++) {
        if (strcmp(vm->vars[i].name, name) == 0) {
            vm->vars[i].value = value;
            return;
        }
    }
    if (vm->var_count < VARS_MAX) {
        strncpy(vm->vars[vm->var_count].name, name, 63);
        vm->vars[vm->var_count].value = value;
        vm->var_count++;
    }
}

VMExecutionTrace* vm_execute(BytecodeChunk* chunk) {
    VMExecutionTrace* trace = (VMExecutionTrace*)malloc(sizeof(VMExecutionTrace));
    trace->capacity = 64;
    trace->count = 0;
    trace->steps = (VMStep*)malloc(sizeof(VMStep) * trace->capacity);

    if (!chunk || chunk->count == 0) return trace;

    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm.chunk = chunk;
    vm.pc = 0;
    vm.sp = 0;

    while (vm.pc < chunk->count) {
        Instruction instr = chunk->code[vm.pc];

        // Record trace step
        if (trace->count >= trace->capacity) {
            trace->capacity *= 2;
            trace->steps = (VMStep*)realloc(trace->steps, sizeof(VMStep) * trace->capacity);
        }

        VMStep* step = &trace->steps[trace->count++];
        memset(step, 0, sizeof(VMStep));
        step->pc = vm.pc;
        step->line = instr.line;
        step->stack_top = vm.sp;
        for (int i = 0; i < vm.sp; i++) step->operand_stack[i] = vm.stack[i];
        step->var_count = vm.var_count;
        for (int i = 0; i < vm.var_count; i++) step->variables[i] = vm.vars[i];
        strncpy(step->console_output, vm.console, 2047);

        // Execute instruction
        switch (instr.op) {
            case OP_PUSH:
                if (vm.sp < STACK_MAX) vm.stack[vm.sp++] = instr.operand;
                break;
            case OP_LOAD:
                if (vm.sp < STACK_MAX) vm.stack[vm.sp++] = get_var(&vm, instr.symbol);
                break;
            case OP_STORE:
                if (vm.sp > 0) set_var(&vm, instr.symbol, vm.stack[--vm.sp]);
                break;
            case OP_ADD:
                if (vm.sp >= 2) {
                    int b = vm.stack[--vm.sp];
                    int a = vm.stack[--vm.sp];
                    vm.stack[vm.sp++] = a + b;
                }
                break;
            case OP_SUB:
                if (vm.sp >= 2) {
                    int b = vm.stack[--vm.sp];
                    int a = vm.stack[--vm.sp];
                    vm.stack[vm.sp++] = a - b;
                }
                break;
            case OP_MUL:
                if (vm.sp >= 2) {
                    int b = vm.stack[--vm.sp];
                    int a = vm.stack[--vm.sp];
                    vm.stack[vm.sp++] = a * b;
                }
                break;
            case OP_DIV:
                if (vm.sp >= 2) {
                    int b = vm.stack[--vm.sp];
                    int a = vm.stack[--vm.sp];
                    vm.stack[vm.sp++] = b != 0 ? a / b : 0;
                }
                break;
            case OP_PRINT:
                if (vm.sp > 0) {
                    char temp[128];
                    snprintf(temp, sizeof(temp), "Output: %d\n", vm.stack[vm.sp - 1]);
                    strncat(vm.console, temp, sizeof(vm.console) - strlen(vm.console) - 1);
                } else {
                    strncat(vm.console, "Program executed successfully.\n", sizeof(vm.console) - strlen(vm.console) - 1);
                }
                break;
            case OP_HALT:
                vm.pc = chunk->count;
                continue;
            default:
                break;
        }

        vm.pc++;
    }

    return trace;
}

void vm_trace_free(VMExecutionTrace* trace) {
    if (trace) {
        if (trace->steps) free(trace->steps);
        free(trace);
    }
}

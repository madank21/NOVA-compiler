#ifndef NOVA_VM_H
#define NOVA_VM_H

#include "bytecode.h"

#define VM_MEM_MAX 65536
#define VM_STACK_MAX 4096
#define VM_CALL_DEPTH_MAX 1024
#define VM_MAX_STEPS 200000
#define VM_TRACE_MAX_STEPS 2000

typedef struct {
    char name[256];
    double value;
} VMVarSnap;

typedef struct {
    char func[260];
    char ret_addr[16];
} VMFrameSnap;

typedef struct {
    int step;
    int pc;
    int line;
    char* instruction;
    double* stack;
    int stack_count;
    VMVarSnap* variables;
    int var_count;
    VMFrameSnap* frames;
    int frame_count;
    char* console;
} VMStep;

typedef struct {
    VMStep* steps;
    int count;
    int capacity;
    int truncated;
    char* console_output;   /* final output (owned) */
    int waiting_for_input;
    char input_prompt[256];
    int exit_code;
    DiagList* runtime_diags; /* borrowed, diags appended here */
} VMResult;

VMResult* vm_execute(const BytecodeChunk* chunk, SemModel* sem,
                     const char** inputs, int input_count, DiagList* diags);
void vm_result_free(VMResult* r);

#endif

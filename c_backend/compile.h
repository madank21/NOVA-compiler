#ifndef NOVA_COMPILE_H
#define NOVA_COMPILE_H

#include "lexer.h"
#include "parser.h"
#include <stddef.h>

/* ----------------------------- Symbol table ------------------------------ */

typedef struct {
    char scope[128];
    char name[256];
    char kind[32];      /* Function, Variable, Parameter, Array, Struct, ... */
    char type[96];
    char address[16];
    int params;         /* -1 for variadic */
} SymRow;

typedef struct {
    char name[256];
    char type[96];
    int is_array;
    int size;
    int isGlobal;
    int offset;
    int isParam;
} SymRec;

typedef struct {
    SymRec *items;
    int count;
    int capacity;
} SymRecList;

typedef struct {
    char name[128];
    char type[96];
    int offset;
    int size;
    int is_array;
} StructField;

typedef struct {
    char name[128];
    StructField *fields;
    int field_count;
    int field_capacity;
    int size;
} StructDef;

typedef struct {
    char name[256];
    char type_name[96];
    NovaNode *node;
    SymRecList frame;   /* params + locals, slot order */
    int param_count;
    int frame_size;
    int is_forward;
} FuncDef;

typedef struct {
    SymRow *symbols; int symbol_count, symbol_capacity;
    SymRecList globals;
    FuncDef *functions; int func_count, func_capacity;
    StructDef *structs; int struct_count, struct_capacity;
    NovaNode **staticDecls; int static_count, static_capacity;
    int globalSlotCount;
    DiagList *diags;
} SemResult;

SemResult *nova_semantic(NovaNode *ast, DiagList *diags);
void nova_semantic_free(SemResult *s);
SymRec *sem_find_global(SemResult *s, const char *name);
FuncDef *sem_find_function(SemResult *s, const char *name);
StructDef *sem_find_struct(SemResult *s, const char *name);
SymRec *sem_find_in_frame(FuncDef *f, const char *name);

/* --------------------------------- TAC ----------------------------------- */

typedef struct {
    char op[12];
    char res[64];
    char a1[64];
    char a2[64];
    int line;
} TacInstr;

typedef struct {
    TacInstr *items; int count, capacity;
    char **strings; int string_count, string_capacity;
    int tempCount;
    int labelCount;
    /* tempTypes: parallel arrays */
    char (*tempNames)[32]; char (*tempTypes)[16]; int tempType_count, tempType_capacity;
} TacResult;

TacResult *nova_gen_tac(NovaNode *ast, SemResult *sem, DiagList *diags);
void nova_tac_free(TacResult *t);

/* ------------------------------- Optimizer -------------------------------- */

typedef struct {
    int constant_fold;
    int constant_prop;
    int dead_code;
    int strength_reduce;
    double reduction_percentage;
} OptMetrics;

TacResult *nova_optimize(TacResult *in, OptMetrics *metrics);

/* ------------------------------- Bytecode --------------------------------- */

typedef struct {
    int pc;
    char op[12];
    double operand;
    char symbol[64];
    int line;
    int slot;
    int isGlobal;
    int fmtIdx;
} BcInstr;

typedef struct {
    BcInstr *items; int count, capacity;
    char **strings; int string_count; /* borrowed from TAC */
    /* funcPC */
    char (*funcNames)[256]; int *funcPCs; int func_count, func_capacity;
    /* tempsByFunc */
    char (*tbfNames)[256]; int *tbfTemps; int tbf_count, tbf_capacity;
} BcResult;

BcResult *nova_gen_bytecode(TacResult *tac, SemResult *sem);
void nova_bytecode_free(BcResult *b);
int bc_func_pc(BcResult *b, const char *name);
int bc_temps_for(BcResult *b, const char *name);

/* ---------------------------------- VM ------------------------------------ */

typedef struct {
    int pc;
    int line;
    char instruction[160];
    double *stack; int stack_count;
    struct { char name[256]; double value; } *variables; int var_count;
    struct { char func[260]; char retAddr[16]; } *frames; int frame_count;
    char *console;
} VMStep;

typedef struct {
    VMStep *steps; int count, capacity;
    int truncated;
    char *consoleOutput;
    int waitingForInput;
    char inputPrompt[256];
    DiagList *runtimeDiags; /* owned */
    int exitCode;
} VMResult;

VMResult *nova_vm_run(BcResult *bc, SemResult *sem, const char **inputs, int input_count);
void nova_vm_free(VMResult *v);

/* ------------------------------ CompileResult ----------------------------- */

typedef struct {
    int success;
    const char *engine;
    double compile_time_ms;
    NovaTokenList *tokens;
    NovaNode *ast;
    SemResult *sem;
    TacResult *tac;
    TacResult *optTac;
    OptMetrics metrics;
    BcResult *bytecode;
    VMResult *vm;
    DiagList *diags;
} CompileResult;

CompileResult *nova_compile(const char *source, const char **inputs, int input_count);
void nova_compile_free(CompileResult *r);
char *nova_to_json(CompileResult *r);

/* Number formatting that matches JS JSON.stringify / String(v):
 * shortest round-trip; integral values print without a decimal point. */
void nova_fmt_shortest(double v, char *out, size_t n);
/* Like nova_fmt_shortest but integral floats keep a trailing ".0"
 * (used for float literal places in TAC, mirroring the JS constString). */
void nova_fmt_float_const(double v, char *out, size_t n);

#endif

#ifndef NOVA_BYTECODE_H
#define NOVA_BYTECODE_H

#include "tac.h"
#include "semantic.h"

/* Serialized JSON fields: pc, op, operand, symbol, line.
 * The remaining fields are internal (slot layout, fixups) and are identical
 * across the JS and C engines. */
typedef struct {
    int pc;
    char op[12];
    double operand;
    char symbol[64];
    int line;
    /* internal */
    int slot;
    int is_global;
    int fmt_idx;
    int array_size;
    char target_label[64];
    int has_fixup;
} BInstr;

typedef struct { char name[256]; int pc; } FuncEntry;
typedef struct { char name[64]; int pc; } LabelEntry;
typedef struct { char name[256]; int temps; } TempsEntry;

typedef struct {
    BInstr* code;
    int count;
    int capacity;
    const StrList* strings;   /* borrowed from TACGen */
    FuncEntry* funcs;   int func_count, func_cap;
    LabelEntry* labels; int label_count, label_cap;
    TempsEntry* temps_by_func; int tbf_count, tbf_cap;
} BytecodeChunk;

BytecodeChunk* generate_bytecode(const TACList* opt_tac, SemModel* sem,
                                 const TempTypeList* temp_types, const StrList* strings,
                                 DiagList* diags);
void bytecode_chunk_free(BytecodeChunk* chunk);
int chunk_func_pc(const BytecodeChunk* chunk, const char* name);
int chunk_temps_for(const BytecodeChunk* chunk, const char* name);

#endif
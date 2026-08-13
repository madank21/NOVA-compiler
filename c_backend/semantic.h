#ifndef NOVA_SEMANTIC_H
#define NOVA_SEMANTIC_H

#include "parser.h"

/* Memory/symbol record for one variable, array, or parameter. */
typedef struct {
    char name[256];
    char type[64];
    int is_array;
    int size;          /* slots */
    int is_global;
    int offset;        /* global slot or frame slot */
    int is_param;
    int is_phantom;    /* placeholder emitted after an undefined-identifier error */
} SymRec;

typedef struct {
    SymRec* items;
    int count;
    int capacity;
} RecList;

typedef struct {
    char name[256];
    char type_name[64];
    ASTNode* node;
    RecList frame;     /* params + locals in slot order */
    int param_count;
    int frame_size;
} FuncDef;

typedef struct {
    char name[256];
    char type[64];
    int offset;
    int size;
    int is_array;
} FieldDef;

typedef struct {
    char name[256];
    FieldDef* fields;
    int field_count;
    int field_capacity;
    int size;
} StructDef;

/* One row of the serialized symbol table (order matters). */
typedef struct {
    char scope[256];
    char name[256];
    const char* kind;
    char type[64];
    char address[16];
    int params;
} SymbolRow;

typedef struct {
    DiagList* diags;
    StrPool* pool;
    RecList globals;
    FuncDef* functions;  int func_count, func_capacity;
    StructDef* structs;  int struct_count, struct_capacity;
    SymbolRow* symbols;  int symbol_count, symbol_capacity;
    int global_slot_count;
} SemModel;

SemModel* analyze_semantics(ASTNode* ast, DiagList* diags, StrPool* pool);
void sem_model_free(SemModel* m);

SymRec* reclist_find_pub(RecList* l, const char* name);

RecList* sem_find_function(SemModel* m, const char* name);
FuncDef* sem_get_function(SemModel* m, const char* name);
SymRec* sem_find_global(SemModel* m, const char* name);
SymRec* sem_find_in_frame(RecList* frame, const char* name);
StructDef* sem_get_struct(SemModel* m, const char* name);

#endif

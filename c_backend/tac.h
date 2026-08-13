#ifndef NOVA_TAC_H
#define NOVA_TAC_H

#include "parser.h"
#include "semantic.h"

typedef struct {
    char op[12];
    char res[64];
    char a1[64];
    char a2[64];
    int line;
} TACInstr;

typedef struct {
    TACInstr* items;
    int count;
    int capacity;
} TACList;

/* temp name -> declared type ("int" / "double" / "ptr") */
typedef struct {
    char name[32];
    char type[16];
} TempType;

typedef struct {
    TempType* items;
    int count;
    int capacity;
} TempTypeList;

typedef struct {
    char** items;
    int count;
    int capacity;
} StrList;

typedef struct {
    TACList* instrs;
    TempTypeList temp_types;
    StrList strings; /* string constant pool (printf formats) */
    int temp_count;
    int label_count;
} TACGen;

TACGen* generate_tac(ASTNode* ast, SemModel* sem, DiagList* diags, StrPool* pool);
void tac_gen_free(TACGen* g);
void tac_list_free(TACList* list);
TACList* tac_list_clone(const TACList* list);
const char* temp_type_of(const TempTypeList* l, const char* name);

#endif
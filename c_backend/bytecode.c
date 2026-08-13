#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void* xmalloc(size_t n) { void* p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void* xrealloc(void* p, size_t n) { void* q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }
static void* xcalloc(size_t n, size_t s) { void* p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }

/* temp-slot map used during codegen */
typedef struct { char name[64]; int offset; } TempSlot;
typedef struct {
    char func[256];
    TempSlot* items;
    int count;
    int capacity;
} FuncTemps;

typedef struct {
    FuncTemps* items;
    int count;
    int capacity;
} FuncTempsList;

static FuncTemps* ftemps_for(FuncTempsList* l, const char* func) {
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].func, func) == 0) return &l->items[i];
    }
    if (l->count >= l->capacity) {
        l->capacity = l->capacity ? l->capacity * 2 : 8;
        l->items = (FuncTemps*)xrealloc(l->items, sizeof(FuncTemps) * (size_t)l->capacity);
    }
    FuncTemps* ft = &l->items[l->count++];
    memset(ft, 0, sizeof(FuncTemps));
    snprintf(ft->func, sizeof(ft->func), "%s", func);
    ft->capacity = 16;
    ft->items = (TempSlot*)xmalloc(sizeof(TempSlot) * (size_t)ft->capacity);
    return ft;
}

typedef struct {
    int is_global;
    int slot;
    int size;
} PlaceInfo;

typedef struct {
    BytecodeChunk* chunk;
    SemModel* sem;
    const TempTypeList* temp_types;
    DiagList* diags;
    char current_func[256];
    FuncTempsList ftemps;
    FuncDef* current_fdef;
} BC;

static BInstr* emit_instr(BC* bc, const char* op, double operand, const char* symbol, int line) {
    BytecodeChunk* c = bc->chunk;
    if (c->count >= c->capacity) {
        c->capacity *= 2;
        c->code = (BInstr*)xrealloc(c->code, sizeof(BInstr) * (size_t)c->capacity);
    }
    BInstr* ins = &c->code[c->count];
    memset(ins, 0, sizeof(BInstr));
    ins->pc = c->count;
    snprintf(ins->op, sizeof(ins->op), "%s", op);
    ins->operand = operand;
    if (symbol) snprintf(ins->symbol, sizeof(ins->symbol), "%s", symbol);
    ins->line = line;
    c->count++;
    return ins;
}

static int is_numeric_place(const char* p) {
    if (!p || !*p) return 0;
    int i = (p[0] == '-') ? 1 : 0;
    int digits = 0, dot = 0;
    for (; p[i]; i++) {
        if (isdigit((unsigned char)p[i])) digits++;
        else if (p[i] == '.' && !dot) dot = 1;
        else return 0;
    }
    return digits > 0;
}

static int is_float_literal_place(const char* p) {
    /* matches JS /^-?\d+\.\d+$/ */
    if (!p) return 0;
    int i = (p[0] == '-') ? 1 : 0;
    int d1 = 0;
    while (isdigit((unsigned char)p[i])) { d1++; i++; }
    if (!d1 || p[i] != '.') return 0;
    i++;
    int d2 = 0;
    while (isdigit((unsigned char)p[i])) { d2++; i++; }
    return d2 > 0 && p[i] == '\0';
}

static PlaceInfo place_info(BC* bc, const char* place) {
    PlaceInfo info;
    memset(&info, 0, sizeof(info));
    SymRec* g = sem_find_global(bc->sem, place);
    if (g) {
        info.is_global = 1;
        info.slot = g->offset;
        info.size = g->size;
        return info;
    }
    if (bc->current_fdef) {
        SymRec* r = reclist_find_pub(&bc->current_fdef->frame, place);
        if (r) {
            info.is_global = 0;
            info.slot = r->offset;
            info.size = r->size;
            return info;
        }
        FuncTemps* ft = ftemps_for(&bc->ftemps, bc->current_func);
        for (int i = 0; i < ft->count; i++) {
            if (strcmp(ft->items[i].name, place) == 0) {
                info.is_global = 0;
                info.slot = ft->items[i].offset;
                info.size = 1;
                return info;
            }
        }
        if (ft->count >= ft->capacity) {
            ft->capacity *= 2;
            ft->items = (TempSlot*)xrealloc(ft->items, sizeof(TempSlot) * (size_t)ft->capacity);
        }
        TempSlot* ts = &ft->items[ft->count++];
        snprintf(ts->name, sizeof(ts->name), "%s", place);
        ts->name[sizeof(ts->name) - 1] = '\0';
        ts->offset = bc->current_fdef->frame_size + (ft->count - 1);
        info.is_global = 0;
        info.slot = ts->offset;
        info.size = 1;
        return info;
    }
    info.is_global = 1;
    info.slot = 0;
    info.size = 1;
    return info;
}

static void push_place(BC* bc, const char* place, int line) {
    if (is_numeric_place(place)) {
        emit_instr(bc, "PUSH", strtod(place, NULL), place, line);
    } else if (strncmp(place, "\"str", 4) == 0) {
        char buf[32];
        size_t n = strlen(place);
        if (n > 5) {
            snprintf(buf, sizeof(buf), "%s", place + 4);
            buf[sizeof(buf) - 1] = '\0';
            char* end = strrchr(buf, '"');
            if (end) *end = '\0';
        } else {
            buf[0] = '0'; buf[1] = '\0';
        }
        emit_instr(bc, "PUSH_STR", (double)atoi(buf), place, line);
    } else {
        PlaceInfo info = place_info(bc, place);
        BInstr* ins = emit_instr(bc, "LOAD", 0, place, line);
        ins->slot = info.slot;
        ins->is_global = info.is_global;
    }
}

static int array_size_of(BC* bc, const char* name) {
    SymRec* g = sem_find_global(bc->sem, name);
    if (g) return g->is_array ? g->size : -1;
    if (bc->current_fdef) {
        SymRec* r = reclist_find_pub(&bc->current_fdef->frame, name);
        if (r) return r->is_array ? r->size : -1;
    }
    return -1;
}

static void add_fixup(BInstr* ins, const char* label) {
    ins->has_fixup = 1;
    snprintf(ins->target_label, sizeof(ins->target_label), "%s", label);
}

static int label_pc(BytecodeChunk* c, const char* name) {
    for (int i = 0; i < c->label_count; i++) {
        if (strcmp(c->labels[i].name, name) == 0) return c->labels[i].pc;
    }
    return -1;
}

static void store_result(BC* bc, const char* res, int line) {
    PlaceInfo info = place_info(bc, res);
    BInstr* ins = emit_instr(bc, "STORE", 0, res, line);
    ins->slot = info.slot;
    ins->is_global = info.is_global;
}

BytecodeChunk* generate_bytecode(const TACList* opt_tac, SemModel* sem,
                                 const TempTypeList* temp_types, const StrList* strings,
                                 DiagList* diags) {
    BC bc;
    memset(&bc, 0, sizeof(BC));
    bc.sem = sem;
    bc.temp_types = temp_types;
    bc.diags = diags;

    BytecodeChunk* c = (BytecodeChunk*)xcalloc(1, sizeof(BytecodeChunk));
    c->capacity = 256;
    c->code = (BInstr*)xmalloc(sizeof(BInstr) * (size_t)c->capacity);
    c->strings = strings;
    c->func_cap = 8;
    c->funcs = (FuncEntry*)xmalloc(sizeof(FuncEntry) * (size_t)c->func_cap);
    c->label_cap = 64;
    c->labels = (LabelEntry*)xmalloc(sizeof(LabelEntry) * (size_t)c->label_cap);
    c->tbf_cap = 8;
    c->temps_by_func = (TempsEntry*)xmalloc(sizeof(TempsEntry) * (size_t)c->tbf_cap);
    bc.chunk = c;

    for (int i = 0; i < opt_tac->count; i++) {
        const TACInstr* ins = &opt_tac->items[i];
        const char* op = ins->op;

        if (strcmp(op, "FUNC_BEGIN") == 0) {
            snprintf(bc.current_func, sizeof(bc.current_func), "%s", ins->res);
            bc.current_func[sizeof(bc.current_func) - 1] = '\0';
            bc.current_fdef = sem_get_function(sem, bc.current_func);
            if (c->func_count >= c->func_cap) {
                c->func_cap *= 2;
                c->funcs = (FuncEntry*)xrealloc(c->funcs, sizeof(FuncEntry) * (size_t)c->func_cap);
            }
            snprintf(c->funcs[c->func_count].name, sizeof(c->funcs[0].name), "%s", ins->res);
            c->funcs[c->func_count].pc = c->count;
            c->func_count++;
        } else if (strcmp(op, "FUNC_END") == 0) {
            FuncTemps* ft = ftemps_for(&bc.ftemps, ins->res);
            if (c->tbf_count >= c->tbf_cap) {
                c->tbf_cap *= 2;
                c->temps_by_func = (TempsEntry*)xrealloc(c->temps_by_func, sizeof(TempsEntry) * (size_t)c->tbf_cap);
            }
            snprintf(c->temps_by_func[c->tbf_count].name, sizeof(c->temps_by_func[0].name), "%s", ins->res);
            c->temps_by_func[c->tbf_count].temps = ft->count;
            c->tbf_count++;
            bc.current_func[0] = '\0';
            bc.current_fdef = NULL;
        } else if (strcmp(op, "LABEL") == 0) {
            if (c->label_count >= c->label_cap) {
                c->label_cap *= 2;
                c->labels = (LabelEntry*)xrealloc(c->labels, sizeof(LabelEntry) * (size_t)c->label_cap);
            }
            snprintf(c->labels[c->label_count].name, sizeof(c->labels[0].name), "%s", ins->res);
            c->labels[c->label_count].pc = c->count;
            c->label_count++;
        } else if (strcmp(op, "GOTO") == 0) {
            BInstr* b = emit_instr(&bc, "JMP", 0, ins->res, ins->line);
            add_fixup(b, ins->res);
        } else if (strcmp(op, "IF_FALSE") == 0) {
            push_place(&bc, ins->a1, ins->line);
            BInstr* b = emit_instr(&bc, "JZ", 0, ins->res, ins->line);
            add_fixup(b, ins->res);
        } else if (strcmp(op, "=") == 0) {
            push_place(&bc, ins->a1, ins->line);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
                   strcmp(op, "/") == 0 || strcmp(op, "%") == 0 || strcmp(op, "==") == 0 ||
                   strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                   strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 || strcmp(op, "&&") == 0 ||
                   strcmp(op, "||") == 0) {
            push_place(&bc, ins->a1, ins->line);
            push_place(&bc, ins->a2, ins->line);
            const char* bop = "ADD";
            if (strcmp(op, "+") == 0) bop = "ADD";
            else if (strcmp(op, "-") == 0) bop = "SUB";
            else if (strcmp(op, "*") == 0) bop = "MUL";
            else if (strcmp(op, "%") == 0) bop = "MOD";
            else if (strcmp(op, "==") == 0) bop = "EQ";
            else if (strcmp(op, "!=") == 0) bop = "NEQ";
            else if (strcmp(op, "<") == 0) bop = "LT";
            else if (strcmp(op, ">") == 0) bop = "GT";
            else if (strcmp(op, "<=") == 0) bop = "LEQ";
            else if (strcmp(op, ">=") == 0) bop = "GEQ";
            else if (strcmp(op, "&&") == 0) bop = "AND";
            else if (strcmp(op, "||") == 0) bop = "OR";
            if (strcmp(op, "/") == 0) {
                const char* lt = temp_type_of(temp_types, ins->a1);
                const char* rt = temp_type_of(temp_types, ins->a2);
                int floaty = is_float_literal_place(ins->a1) || is_float_literal_place(ins->a2) ||
                             (lt && strcmp(lt, "double") == 0) || (rt && strcmp(rt, "double") == 0);
                bop = floaty ? "DIVF" : "DIV";
            }
            emit_instr(&bc, bop, 0, ins->res, ins->line);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "neg") == 0) {
            push_place(&bc, ins->a1, ins->line);
            emit_instr(&bc, "NEG", 0, ins->res, ins->line);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "!") == 0) {
            push_place(&bc, ins->a1, ins->line);
            emit_instr(&bc, "NOT", 0, ins->res, ins->line);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "ADDR") == 0) {
            PlaceInfo info = place_info(&bc, ins->a1);
            long long off = 0;
            if (ins->a2[0]) off = strtoll(ins->a2, NULL, 10);
            BInstr* b = emit_instr(&bc, "ADDR", (double)off, ins->a1, ins->line);
            b->slot = info.slot;
            b->is_global = info.is_global;
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "IDX_ADDR") == 0) {
            PlaceInfo info = place_info(&bc, ins->a1);
            push_place(&bc, ins->a2, ins->line); /* index */
            BInstr* b = emit_instr(&bc, "IDX_ADDR", (double)array_size_of(&bc, ins->a1), ins->a1, ins->line);
            b->slot = info.slot;
            b->is_global = info.is_global;
            b->array_size = array_size_of(&bc, ins->a1);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "LOAD_PTR") == 0) {
            push_place(&bc, ins->a1, ins->line);
            emit_instr(&bc, "LOAD_AT", 0, ins->res, ins->line);
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "STORE_PTR") == 0) {
            /* a1 = address place, a2 = value place */
            push_place(&bc, ins->a2, ins->line);
            push_place(&bc, ins->a1, ins->line);
            emit_instr(&bc, "STORE_AT", 0, ins->a1, ins->line);
        } else if (strcmp(op, "PARAM") == 0) {
            push_place(&bc, ins->a1, ins->line);
        } else if (strcmp(op, "CALL") == 0) {
            BInstr* b = emit_instr(&bc, "CALL", (double)atoi(ins->a2), ins->a1, ins->line);
            (void)b;
            store_result(&bc, ins->res, ins->line);
        } else if (strcmp(op, "PRINT") == 0) {
            char buf[32];
            size_t n = strlen(ins->a1);
            snprintf(buf, sizeof(buf), "%s", n > 4 ? ins->a1 + 4 : "0");
            buf[sizeof(buf) - 1] = '\0';
            char* end = strrchr(buf, '"');
            if (end) *end = '\0';
            BInstr* b = emit_instr(&bc, "PRINT", (double)atoi(ins->a2), ins->a1, ins->line);
            b->fmt_idx = atoi(buf);
        } else if (strcmp(op, "READ") == 0) {
            char buf[32];
            size_t n = strlen(ins->a1);
            snprintf(buf, sizeof(buf), "%s", n > 4 ? ins->a1 + 4 : "0");
            buf[sizeof(buf) - 1] = '\0';
            char* end = strrchr(buf, '"');
            if (end) *end = '\0';
            BInstr* b = emit_instr(&bc, "INPUT", (double)atoi(ins->a2), ins->a1, ins->line);
            b->fmt_idx = atoi(buf);
        } else if (strcmp(op, "RETURN") == 0) {
            int has_val = ins->a1[0] != '\0';
            if (has_val) push_place(&bc, ins->a1, ins->line);
            emit_instr(&bc, "RET", has_val ? 1.0 : 0.0, "", ins->line);
        }
    }

    emit_instr(&bc, "HALT", 0, "", 0);

    /* fixups */
    for (int i = 0; i < c->count; i++) {
        BInstr* ins = &c->code[i];
        if (ins->has_fixup) {
            int target = label_pc(c, ins->target_label);
            ins->operand = target >= 0 ? target : c->count - 1;
        }
    }

    /* free temp-slot maps */
    for (int i = 0; i < bc.ftemps.count; i++) free(bc.ftemps.items[i].items);
    free(bc.ftemps.items);

    return c;
}

int chunk_func_pc(const BytecodeChunk* chunk, const char* name) {
    for (int i = 0; i < chunk->func_count; i++) {
        if (strcmp(chunk->funcs[i].name, name) == 0) return chunk->funcs[i].pc;
    }
    return -1;
}

int chunk_temps_for(const BytecodeChunk* chunk, const char* name) {
    for (int i = 0; i < chunk->tbf_count; i++) {
        if (strcmp(chunk->temps_by_func[i].name, name) == 0) return chunk->temps_by_func[i].temps;
    }
    return 0;
}

void bytecode_chunk_free(BytecodeChunk* chunk) {
    if (!chunk) return;
    free(chunk->code);
    free(chunk->funcs);
    free(chunk->labels);
    free(chunk->temps_by_func);
    free(chunk);
}

#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }

typedef struct {
    BcResult *r;
    SemResult *sem;
    char currentFunc[256];
    /* labelPC */
    char (*labelNames)[64]; int *labelPCs; int label_count, label_capacity;
    /* fixups */
    struct { int index; char label[64]; } *fixups; int fixup_count, fixup_capacity;
    /* tempSlots per function */
    struct { char func[256]; char (*names)[64]; int *slots; int count, capacity; } *funcTemps; int funcTemp_count, funcTemp_capacity;
} BG;

static int is_numeric_place(const char *p) {
    if (!p || !*p) return 0;
    int i = (p[0] == '-') ? 1 : 0;
    int digits = 0, dots = 0;
    for (; p[i]; i++) {
        if (isdigit((unsigned char)p[i])) digits++;
        else if (p[i] == '.' && dots == 0) dots++;
        else return 0;
    }
    return digits > 0;
}

static BcInstr *emit_instr(BG *bg, const char *op, double operand, const char *symbol, int line) {
    BcResult *r = bg->r;
    if (r->count >= r->capacity) {
        r->capacity = r->capacity ? r->capacity * 2 : 128;
        r->items = (BcInstr *)xrealloc(r->items, sizeof(BcInstr) * (size_t)r->capacity);
    }
    BcInstr *ins = &r->items[r->count];
    memset(ins, 0, sizeof(BcInstr));
    ins->pc = r->count;
    strncpy(ins->op, op, sizeof(ins->op) - 1);
    ins->operand = operand;
    if (symbol) strncpy(ins->symbol, symbol, sizeof(ins->symbol) - 1);
    ins->line = line;
    r->count++;
    return ins;
}

typedef struct { int isGlobal; int slot; int size; } PlaceInfo;

static PlaceInfo place_info(BG *bg, const char *place) {
    PlaceInfo info; memset(&info, 0, sizeof(info));
    SymRec *g = sem_find_global(bg->sem, place);
    if (g) { info.isGlobal = 1; info.slot = g->offset; info.size = g->size; return info; }
    FuncDef *f = sem_find_function(bg->sem, bg->currentFunc);
    if (f) {
        SymRec *r = sem_find_in_frame(f, place);
        if (r) { info.isGlobal = 0; info.slot = r->offset; info.size = r->size; return info; }
        /* temp slot */
        int ftIdx = -1;
        for (int i = 0; i < bg->funcTemp_count; i++) {
            if (strcmp(bg->funcTemps[i].func, bg->currentFunc) == 0) { ftIdx = i; break; }
        }
        if (ftIdx < 0) {
            if (bg->funcTemp_count >= bg->funcTemp_capacity) {
                bg->funcTemp_capacity = bg->funcTemp_capacity ? bg->funcTemp_capacity * 2 : 8;
                bg->funcTemps = xrealloc(bg->funcTemps, sizeof(*bg->funcTemps) * (size_t)bg->funcTemp_capacity);
            }
            ftIdx = bg->funcTemp_count++;
            memset(&bg->funcTemps[ftIdx], 0, sizeof(bg->funcTemps[ftIdx]));
            strncpy(bg->funcTemps[ftIdx].func, bg->currentFunc, 255);
        }
        for (int i = 0; i < bg->funcTemps[ftIdx].count; i++) {
            if (strcmp(bg->funcTemps[ftIdx].names[i], place) == 0) {
                info.isGlobal = 0; info.slot = bg->funcTemps[ftIdx].slots[i]; info.size = 1;
                return info;
            }
        }
        if (bg->funcTemps[ftIdx].count >= bg->funcTemps[ftIdx].capacity) {
            bg->funcTemps[ftIdx].capacity = bg->funcTemps[ftIdx].capacity ? bg->funcTemps[ftIdx].capacity * 2 : 16;
            bg->funcTemps[ftIdx].names = (char (*)[64])xrealloc(bg->funcTemps[ftIdx].names, sizeof(char[64]) * (size_t)bg->funcTemps[ftIdx].capacity);
            bg->funcTemps[ftIdx].slots = (int *)xrealloc(bg->funcTemps[ftIdx].slots, sizeof(int) * (size_t)bg->funcTemps[ftIdx].capacity);
        }
        int slot = f->frame_size + bg->funcTemps[ftIdx].count;
        strncpy(bg->funcTemps[ftIdx].names[bg->funcTemps[ftIdx].count], place, 63);
        bg->funcTemps[ftIdx].slots[bg->funcTemps[ftIdx].count] = slot;
        bg->funcTemps[ftIdx].count++;
        info.isGlobal = 0; info.slot = slot; info.size = 1;
        return info;
    }
    info.isGlobal = 1; info.slot = 0; info.size = 1;
    return info;
}

static void push_place(BG *bg, const char *place, int line) {
    if (is_numeric_place(place)) {
        emit_instr(bg, "PUSH", atof(place), place, line);
    } else if (strncmp(place, "\"str", 4) == 0) {
        char buf[32];
        size_t n = strlen(place);
        if (n > 5) {
            strncpy(buf, place + 4, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *end = strrchr(buf, '"');
            if (end) *end = '\0';
        } else { buf[0] = '0'; buf[1] = '\0'; }
        emit_instr(bg, "PUSH_STR", (double)atoi(buf), place, line);
    } else {
        PlaceInfo info = place_info(bg, place);
        BcInstr *ins = emit_instr(bg, "LOAD", 0, place, line);
        ins->slot = info.slot;
        ins->isGlobal = info.isGlobal;
    }
}

static int array_size_of(BG *bg, const char *name) {
    SymRec *g = sem_find_global(bg->sem, name);
    if (g) return g->is_array ? g->size : -1;
    FuncDef *f = sem_find_function(bg->sem, bg->currentFunc);
    if (f) {
        SymRec *r = sem_find_in_frame(f, name);
        if (r) return r->is_array ? r->size : -1;
    }
    return -1;
}

static void store_result(BG *bg, const char *res, int line) {
    PlaceInfo info = place_info(bg, res);
    BcInstr *ins = emit_instr(bg, "STORE", 0, res, line);
    ins->slot = info.slot;
    ins->isGlobal = info.isGlobal;
}

static void add_fixup(BG *bg, int index, const char *label) {
    if (bg->fixup_count >= bg->fixup_capacity) {
        bg->fixup_capacity = bg->fixup_capacity ? bg->fixup_capacity * 2 : 64;
        bg->fixups = xrealloc(bg->fixups, sizeof(*bg->fixups) * (size_t)bg->fixup_capacity);
    }
    bg->fixups[bg->fixup_count].index = index;
    strncpy(bg->fixups[bg->fixup_count].label, label, 63);
    bg->fixups[bg->fixup_count].label[63] = '\0';
    bg->fixup_count++;
}

static const char *temp_type_of(TacResult *tac, const char *name) {
    for (int i = 0; i < tac->tempType_count; i++) {
        if (strcmp(tac->tempNames[i], name) == 0) return tac->tempTypes[i];
    }
    return NULL;
}

BcResult *nova_gen_bytecode(TacResult *tac, SemResult *sem) {
    BG bg; memset(&bg, 0, sizeof(bg));
    bg.r = (BcResult *)xcalloc(1, sizeof(BcResult));
    bg.sem = sem;
    bg.r->strings = tac->strings;
    bg.r->string_count = tac->string_count;

    for (int idx = 0; idx < tac->count; idx++) {
        TacInstr *ins = &tac->items[idx];
        if (strcmp(ins->op, "FUNC_BEGIN") == 0) {
            strncpy(bg.currentFunc, ins->res, sizeof(bg.currentFunc) - 1);
            /* funcPC */
            if (bg.r->func_count >= bg.r->func_capacity) {
                bg.r->func_capacity = bg.r->func_capacity ? bg.r->func_capacity * 2 : 16;
                bg.r->funcNames = (char (*)[256])xrealloc(bg.r->funcNames, sizeof(char[256]) * (size_t)bg.r->func_capacity);
                bg.r->funcPCs = (int *)xrealloc(bg.r->funcPCs, sizeof(int) * (size_t)bg.r->func_capacity);
            }
            strncpy(bg.r->funcNames[bg.r->func_count], ins->res, 255);
            bg.r->funcPCs[bg.r->func_count] = bg.r->count;
            bg.r->func_count++;
        } else if (strcmp(ins->op, "FUNC_END") == 0) {
            /* tempsByFunc */
            int cnt = 0;
            for (int i = 0; i < bg.funcTemp_count; i++) {
                if (strcmp(bg.funcTemps[i].func, bg.currentFunc) == 0) { cnt = bg.funcTemps[i].count; break; }
            }
            if (bg.r->tbf_count >= bg.r->tbf_capacity) {
                bg.r->tbf_capacity = bg.r->tbf_capacity ? bg.r->tbf_capacity * 2 : 16;
                bg.r->tbfNames = (char (*)[256])xrealloc(bg.r->tbfNames, sizeof(char[256]) * (size_t)bg.r->tbf_capacity);
                bg.r->tbfTemps = (int *)xrealloc(bg.r->tbfTemps, sizeof(int) * (size_t)bg.r->tbf_capacity);
            }
            strncpy(bg.r->tbfNames[bg.r->tbf_count], bg.currentFunc, 255);
            bg.r->tbfTemps[bg.r->tbf_count] = cnt;
            bg.r->tbf_count++;
            bg.currentFunc[0] = '\0';
        } else if (strcmp(ins->op, "LABEL") == 0) {
            if (bg.label_count >= bg.label_capacity) {
                bg.label_capacity = bg.label_capacity ? bg.label_capacity * 2 : 64;
                bg.labelNames = (char (*)[64])xrealloc(bg.labelNames, sizeof(char[64]) * (size_t)bg.label_capacity);
                bg.labelPCs = (int *)xrealloc(bg.labelPCs, sizeof(int) * (size_t)bg.label_capacity);
            }
            strncpy(bg.labelNames[bg.label_count], ins->res, 63);
            bg.labelNames[bg.label_count][63] = '\0';
            bg.labelPCs[bg.label_count] = bg.r->count;
            bg.label_count++;
        } else if (strcmp(ins->op, "GOTO") == 0) {
            BcInstr *i = emit_instr(&bg, "JMP", 0, ins->res, ins->line);
            add_fixup(&bg, i->pc, ins->res);
        } else if (strcmp(ins->op, "IF_FALSE") == 0) {
            push_place(&bg, ins->a1, ins->line);
            BcInstr *i = emit_instr(&bg, "JZ", 0, ins->res, ins->line);
            add_fixup(&bg, i->pc, ins->res);
        } else if (strcmp(ins->op, "=") == 0) {
            push_place(&bg, ins->a1, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "+") == 0 || strcmp(ins->op, "-") == 0 || strcmp(ins->op, "*") == 0 ||
                   strcmp(ins->op, "/") == 0 || strcmp(ins->op, "%") == 0 ||
                   strcmp(ins->op, "==") == 0 || strcmp(ins->op, "!=") == 0 ||
                   strcmp(ins->op, "<") == 0 || strcmp(ins->op, ">") == 0 ||
                   strcmp(ins->op, "<=") == 0 || strcmp(ins->op, ">=") == 0 ||
                   strcmp(ins->op, "&&") == 0 || strcmp(ins->op, "||") == 0 ||
                   strcmp(ins->op, "&") == 0 || strcmp(ins->op, "|") == 0 ||
                   strcmp(ins->op, "^") == 0 || strcmp(ins->op, "<<") == 0 || strcmp(ins->op, ">>") == 0) {
            push_place(&bg, ins->a1, ins->line);
            push_place(&bg, ins->a2, ins->line);
            const char *op = NULL;
            if (strcmp(ins->op, "+") == 0) op = "ADD";
            else if (strcmp(ins->op, "-") == 0) op = "SUB";
            else if (strcmp(ins->op, "*") == 0) op = "MUL";
            else if (strcmp(ins->op, "%") == 0) op = "MOD";
            else if (strcmp(ins->op, "==") == 0) op = "EQ";
            else if (strcmp(ins->op, "!=") == 0) op = "NEQ";
            else if (strcmp(ins->op, "<") == 0) op = "LT";
            else if (strcmp(ins->op, ">") == 0) op = "GT";
            else if (strcmp(ins->op, "<=") == 0) op = "LEQ";
            else if (strcmp(ins->op, ">=") == 0) op = "GEQ";
            else if (strcmp(ins->op, "&&") == 0) op = "AND";
            else if (strcmp(ins->op, "||") == 0) op = "OR";
            else if (strcmp(ins->op, "&") == 0) op = "BAND";
            else if (strcmp(ins->op, "|") == 0) op = "BOR";
            else if (strcmp(ins->op, "^") == 0) op = "BXOR";
            else if (strcmp(ins->op, "<<") == 0) op = "SHL";
            else if (strcmp(ins->op, ">>") == 0) op = "SHR";
            if (strcmp(ins->op, "/") == 0) {
                const char *lt = temp_type_of(tac, ins->a1);
                const char *rt = temp_type_of(tac, ins->a2);
                int floaty1 = 0, floaty2 = 0;
                /* numeric literal with '.' counts as float */
                const char *a1 = ins->a1, *a2 = ins->a2;
                if (strchr(a1, '.')) floaty1 = 1;
                if (strchr(a2, '.')) floaty2 = 1;
                if (lt && strcmp(lt, "double") == 0) floaty1 = 1;
                if (rt && strcmp(rt, "double") == 0) floaty2 = 1;
                op = (floaty1 || floaty2) ? "DIVF" : "DIV";
            }
            emit_instr(&bg, op, 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "neg") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "NEG", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "!") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "NOT", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "~") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "BNOT", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "CAST_I") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "CVT_I", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "CAST_F") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "CVT_F", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "ADDR") == 0) {
            PlaceInfo info = place_info(&bg, ins->a1);
            long long off = 0;
            if (ins->a2[0]) off = strtoll(ins->a2, NULL, 10);
            BcInstr *i = emit_instr(&bg, "ADDR", (double)off, ins->a1, ins->line);
            i->slot = info.slot;
            i->isGlobal = info.isGlobal;
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "IDX_ADDR") == 0) {
            PlaceInfo info = place_info(&bg, ins->a1);
            push_place(&bg, ins->a2, ins->line);
            BcInstr *i = emit_instr(&bg, "IDX_ADDR", (double)array_size_of(&bg, ins->a1), ins->a1, ins->line);
            i->slot = info.slot;
            i->isGlobal = info.isGlobal;
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "LOAD_PTR") == 0) {
            push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "LOAD_AT", 0, ins->res, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "STORE_PTR") == 0) {
            push_place(&bg, ins->a2, ins->line); /* value */
            push_place(&bg, ins->a1, ins->line); /* address */
            emit_instr(&bg, "STORE_AT", 0, ins->a1, ins->line);
        } else if (strcmp(ins->op, "PARAM") == 0) {
            push_place(&bg, ins->a1, ins->line);
        } else if (strcmp(ins->op, "CALL") == 0) {
            emit_instr(&bg, "CALL", (double)atoi(ins->a2), ins->a1, ins->line);
            store_result(&bg, ins->res, ins->line);
        } else if (strcmp(ins->op, "PRINT") == 0) {
            char buf[32];
            size_t n = strlen(ins->a1);
            if (n > 5) {
                strncpy(buf, ins->a1 + 4, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *end = strrchr(buf, '"');
                if (end) *end = '\0';
            } else { buf[0] = '0'; buf[1] = '\0'; }
            BcInstr *i = emit_instr(&bg, "PRINT", (double)atoi(ins->a2), ins->a1, ins->line);
            i->fmtIdx = atoi(buf);
        } else if (strcmp(ins->op, "READ") == 0) {
            char buf[32];
            size_t n = strlen(ins->a1);
            if (n > 5) {
                strncpy(buf, ins->a1 + 4, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *end = strrchr(buf, '"');
                if (end) *end = '\0';
            } else { buf[0] = '0'; buf[1] = '\0'; }
            BcInstr *i = emit_instr(&bg, "INPUT", (double)atoi(ins->a2), ins->a1, ins->line);
            i->fmtIdx = atoi(buf);
        } else if (strcmp(ins->op, "RETURN") == 0) {
            int hasVal = (ins->a1[0] != '\0');
            if (hasVal) push_place(&bg, ins->a1, ins->line);
            emit_instr(&bg, "RET", hasVal ? 1 : 0, "", ins->line);
        }
    }

    emit_instr(&bg, "HALT", 0, "", 0);

    /* resolve fixups */
    for (int f = 0; f < bg.fixup_count; f++) {
        int target = -1;
        for (int l = 0; l < bg.label_count; l++) {
            if (strcmp(bg.labelNames[l], bg.fixups[f].label) == 0) { target = bg.labelPCs[l]; break; }
        }
        bg.r->items[bg.fixups[f].index].operand = (target >= 0) ? target : bg.r->count - 1;
    }

    /* free temporaries */
    free(bg.labelNames); free(bg.labelPCs);
    free(bg.fixups);
    for (int i = 0; i < bg.funcTemp_count; i++) { free(bg.funcTemps[i].names); free(bg.funcTemps[i].slots); }
    free(bg.funcTemps);

    return bg.r;
}

int bc_func_pc(BcResult *b, const char *name) {
    for (int i = 0; i < b->func_count; i++) if (strcmp(b->funcNames[i], name) == 0) return b->funcPCs[i];
    return -1;
}
int bc_temps_for(BcResult *b, const char *name) {
    for (int i = 0; i < b->tbf_count; i++) if (strcmp(b->tbfNames[i], name) == 0) return b->tbfTemps[i];
    return 0;
}

void nova_bytecode_free(BcResult *b) {
    if (!b) return;
    free(b->items);
    free(b->funcNames); free(b->funcPCs);
    free(b->tbfNames); free(b->tbfTemps);
    free(b);
}

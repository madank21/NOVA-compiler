#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define VM_MEM_MAX 65536
#define VM_STACK_MAX 4096
#define VM_CALL_DEPTH_MAX 1024
#define VM_MAX_STEPS 200000
#define VM_TRACE_MAX_STEPS 2000

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }
static char *xstrdup(const char *s) { char *d = strdup(s); if (!d) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return d; }

static long long truncate_ll(double v) {
    const double LIM = 9007199254740991.0;
    if (v >= LIM) return 9007199254740991LL;
    if (v <= -LIM) return -9007199254740991LL;
    return (long long)v;
}
static void fmt_number(double v, char *out, size_t n) {
    nova_fmt_shortest(v, out, n);
}

typedef struct { double n; int s; int is_str; } Cell;
typedef struct { char func[256]; int retPC; int bp; } Frame;

typedef struct {
    double *mem;
    Cell *stack; int sp;
    Frame *frames; int frame_count;
    char *console; int console_len, console_cap;
    int *strBase; int strBase_count;
    int randState;
    int halted;
    int waitingForInput;
    char inputPrompt[256];
    int inputIdx;
    const char **inputs; int input_count;
    int exitCode;
    DiagList *runtimeDiags;
    VMResult *result;
    BcResult *bc;
    SemResult *sem;
} VM;

static void vm_runtime_error(VM *vm, const char *msg, int line) {
    diag_add(vm->runtimeDiags, "runtime", line, 0, "%s", msg);
    vm->halted = 1;
}

static void vm_push(VM *vm, double v) {
    if (vm->sp >= VM_STACK_MAX) { vm_runtime_error(vm, "Operand stack overflow", 0); return; }
    vm->stack[vm->sp].n = v;
    vm->stack[vm->sp].is_str = 0;
    vm->sp++;
}
static Cell vm_pop(VM *vm) {
    if (vm->sp > 0) { vm->sp--; return vm->stack[vm->sp]; }
    vm_runtime_error(vm, "Operand stack underflow", 0);
    Cell c; c.n = 0; c.s = 0; c.is_str = 0;
    return c;
}

static void console_append(VM *vm, const char *s) {
    int len = (int)strlen(s);
    while (vm->console_len + len + 1 >= vm->console_cap) {
        vm->console_cap = vm->console_cap ? vm->console_cap * 2 : 256;
        vm->console = (char *)xrealloc(vm->console, (size_t)vm->console_cap);
    }
    memcpy(vm->console + vm->console_len, s, (size_t)len);
    vm->console_len += len;
    vm->console[vm->console_len] = '\0';
}

/* ----------------------------- printf format ----------------------------- */

static unsigned long long to_unsigned32(double v) {
    long long t = truncate_ll(v);
    t %= 4294967296LL;
    if (t < 0) t += 4294967296LL;
    return (unsigned long long)t;
}

static void format_printf(VM *vm, const char *fmt, Cell *args, int nargs) {
    char out[1024];
    int ai = 0;
    size_t flen = strlen(fmt);
    for (size_t i = 0; i < flen; i++) {
        char ch = fmt[i];
        if (ch != '%') {
            char one[2] = { ch, '\0' };
            console_append(vm, one);
            continue;
        }
        i++;
        if (i < flen && fmt[i] == '%') { console_append(vm, "%"); continue; }
        while (i < flen && strchr("-+ #0", fmt[i])) i++;
        while (i < flen && isdigit((unsigned char)fmt[i])) i++;
        int prec = -1;
        if (i < flen && fmt[i] == '.') {
            i++;
            int digits = 0; int hasd = 0;
            while (i < flen && isdigit((unsigned char)fmt[i])) { digits = digits * 10 + (fmt[i] - '0'); hasd = 1; i++; }
            prec = hasd ? digits : 0;
        }
        while (i < flen && strchr("lhztj", fmt[i])) i++;
        char conv = (i < flen) ? fmt[i] : '\0';
        Cell arg;
        if (ai < nargs) arg = args[ai++];
        else { arg.n = 0; arg.s = 0; arg.is_str = 0; }
        double num = arg.n;
        if (conv == 'd' || conv == 'i') {
            snprintf(out, sizeof(out), "%lld", truncate_ll(num));
            console_append(vm, out);
        } else if (conv == 'u') {
            snprintf(out, sizeof(out), "%llu", to_unsigned32(num));
            console_append(vm, out);
        } else if (conv == 'x' || conv == 'X') {
            snprintf(out, sizeof(out), conv == 'x' ? "%llx" : "%llX", to_unsigned32(num));
            console_append(vm, out);
        } else if (conv == 'o') {
            snprintf(out, sizeof(out), "%llo", to_unsigned32(num));
            console_append(vm, out);
        } else if (conv == 'p') {
            snprintf(out, sizeof(out), "0x%llx", to_unsigned32(num));
            console_append(vm, out);
        } else if (conv == 'f') {
            snprintf(out, sizeof(out), "%.*f", prec == -1 ? 6 : prec, num);
            console_append(vm, out);
        } else if (conv == 'e' || conv == 'E') {
            snprintf(out, sizeof(out), conv == 'e' ? "%.*e" : "%.*E", prec == -1 ? 6 : prec, num);
            console_append(vm, out);
        } else if (conv == 'g' || conv == 'G') {
            /* real C %g semantics (precision significant digits, %e style when
             * the exponent is < -4 or >= precision, trailing zeros stripped) */
            snprintf(out, sizeof(out), conv == 'g' ? "%.*g" : "%.*G", prec == -1 ? 6 : prec, num);
            console_append(vm, out);
        } else if (conv == 'c') {
            char one[2] = { (char)(truncate_ll(num) & 0xff), '\0' };
            console_append(vm, one);
        } else if (conv == 's') {
            long long addr = truncate_ll(num);
            char sbuf[4096]; int slen = 0;
            long long guard = 0;
            while (addr >= 0 && addr < VM_MEM_MAX && guard < 4096) {
                double c = vm->mem[addr];
                if (c == 0) break;
                if (slen < 4095) sbuf[slen++] = (char)c;
                addr++; guard++;
            }
            sbuf[slen] = '\0';
            console_append(vm, sbuf);
        } else {
            char one[3] = { '%', conv, '\0' };
            console_append(vm, one);
        }
    }
}

/* Return the next scanf conversion spec in fmt (advancing *pi past it).
 * Literal text and %% are skipped; returns 0 when no more conversions. */
static char scanf_next_conv(const char *fmt, int *pi) {
    size_t n = strlen(fmt);
    while (*pi < (int)n) {
        char ch = fmt[*pi];
        if (ch != '%') { (*pi)++; continue; }
        (*pi)++;
        if (*pi < (int)n && fmt[*pi] == '%') { (*pi)++; continue; } /* %% */
        while (*pi < (int)n && strchr("-+ #0", fmt[*pi])) (*pi)++;
        while (*pi < (int)n && isdigit((unsigned char)fmt[*pi])) (*pi)++;
        if (*pi < (int)n && fmt[*pi] == '.') {
            (*pi)++;
            while (*pi < (int)n && isdigit((unsigned char)fmt[*pi])) (*pi)++;
        }
        while (*pi < (int)n && strchr("lhztj", fmt[*pi])) (*pi)++;
        if (*pi < (int)n) { char c = fmt[*pi]; (*pi)++; return c; }
        return 0;
    }
    return 0;
}

/* ------------------------------- builtins -------------------------------- */

static int call_builtin(VM *vm, BcInstr *instr) {
    const char *name = instr->symbol;
    int nargs = (int)instr->operand;
    if (vm->sp < nargs) { char m[128]; snprintf(m, sizeof(m), "Stack underflow in %s", name); vm_runtime_error(vm, m, instr->line); return 1; }
    double args[16];
    int na = nargs > 16 ? 16 : nargs;
    for (int i = 0; i < na; i++) args[i] = vm->stack[vm->sp - nargs + i].n;
    vm->sp -= nargs;

    if (strcmp(name, "sqrt") == 0) { vm_push(vm, sqrt(args[0])); return 1; }
    if (strcmp(name, "pow") == 0) { vm_push(vm, pow(args[0], args[1])); return 1; }
    if (strcmp(name, "sin") == 0) { vm_push(vm, sin(args[0])); return 1; }
    if (strcmp(name, "cos") == 0) { vm_push(vm, cos(args[0])); return 1; }
    if (strcmp(name, "tan") == 0) { vm_push(vm, tan(args[0])); return 1; }
    if (strcmp(name, "log") == 0) { vm_push(vm, log(args[0])); return 1; }
    if (strcmp(name, "exp") == 0) { vm_push(vm, exp(args[0])); return 1; }
    if (strcmp(name, "ceil") == 0) { vm_push(vm, ceil(args[0])); return 1; }
    if (strcmp(name, "floor") == 0) { vm_push(vm, floor(args[0])); return 1; }
    if (strcmp(name, "fabs") == 0) { vm_push(vm, fabs(args[0])); return 1; }
    if (strcmp(name, "abs") == 0) { vm_push(vm, (double)llabs(truncate_ll(args[0]))); return 1; }
    if (strcmp(name, "fmod") == 0) { vm_push(vm, fmod(args[0], args[1])); return 1; }
    if (strcmp(name, "rand") == 0) {
        vm->randState = (int)((unsigned)vm->randState * 1103515245u + 12345u);
        vm_push(vm, (double)((unsigned)vm->randState >> 16 & 0x7fff));
        return 1;
    }
    if (strcmp(name, "srand") == 0) { vm->randState = (int)truncate_ll(args[0]); vm_push(vm, 0); return 1; }
    if (strcmp(name, "time") == 0) { vm_push(vm, 1700000000.0); return 1; }
    if (strcmp(name, "exit") == 0) { vm->exitCode = (int)truncate_ll(args[0]); vm->halted = 1; return 1; }
    if (strcmp(name, "assert") == 0) {
        if (truncate_ll(args[0]) == 0) vm_runtime_error(vm, "Assertion failed", instr->line);
        vm_push(vm, 0);
        return 1;
    }
    /* not a builtin: restore args */
    for (int i = na - 1; i >= 0; i--) { vm->stack[vm->sp].n = args[i]; vm->stack[vm->sp].is_str = 0; vm->sp++; }
    return 0;
}

/* ------------------------------ trace step -------------------------------- */

static void describe_instr(BcInstr *i, char *out, size_t n) {
    char numbuf[64];
    if (strcmp(i->op, "PUSH") == 0) { fmt_number(i->operand, numbuf, sizeof(numbuf)); snprintf(out, n, "PUSH %s", numbuf); }
    else if (strcmp(i->op, "LOAD") == 0) snprintf(out, n, "LOAD %s", i->symbol);
    else if (strcmp(i->op, "STORE") == 0) snprintf(out, n, "STORE %s", i->symbol);
    else if (strcmp(i->op, "ADDR") == 0) { fmt_number(i->operand, numbuf, sizeof(numbuf)); snprintf(out, n, "ADDR %s+%s", i->symbol, numbuf); }
    else if (strcmp(i->op, "JMP") == 0) snprintf(out, n, "JMP %s", i->symbol);
    else if (strcmp(i->op, "JZ") == 0) snprintf(out, n, "JZ %s", i->symbol);
    else if (strcmp(i->op, "CALL") == 0) snprintf(out, n, "CALL %s (%d args)", i->symbol, (int)i->operand);
    else if (strcmp(i->op, "RET") == 0) snprintf(out, n, "RET");
    else if (strcmp(i->op, "PRINT") == 0) snprintf(out, n, "PRINT");
    else if (strcmp(i->op, "INPUT") == 0) snprintf(out, n, "INPUT");
    else if (strcmp(i->op, "HALT") == 0) snprintf(out, n, "HALT");
    else snprintf(out, n, "%s", i->op);
}

static int frame_size_of(VM *vm, const char *funcName) {
    FuncDef *f = sem_find_function(vm->sem, funcName);
    int base = f ? f->frame_size : 0;
    return base + bc_temps_for(vm->bc, funcName);
}

static void record_step(VM *vm, int pc, BcInstr *instr) {
    VMResult *r = vm->result;
    if (r->count >= VM_TRACE_MAX_STEPS) { r->truncated = 1; return; }
    if (r->count >= r->capacity) {
        r->capacity = r->capacity ? r->capacity * 2 : 256;
        r->steps = (VMStep *)xrealloc(r->steps, sizeof(VMStep) * (size_t)r->capacity);
    }
    VMStep *step = &r->steps[r->count++];
    memset(step, 0, sizeof(VMStep));
    step->pc = pc;
    step->line = instr->line;
    describe_instr(instr, step->instruction, sizeof(step->instruction));

    step->stack_count = vm->sp;
    step->stack = (double *)xmalloc(sizeof(double) * (vm->sp > 0 ? (size_t)vm->sp : 1));
    for (int i = 0; i < vm->sp; i++) step->stack[i] = vm->stack[i].is_str ? 0 : vm->stack[i].n;

    /* variables: globals + current frame locals (non-param), deduplicated by
     * name to match the JS engine's Map-keyed frame (one entry per name, at
     * its first-occurrence position, carrying the last declaration's slot). */
    Frame *frame = vm->frame_count > 0 ? &vm->frames[vm->frame_count - 1] : NULL;
    FuncDef *fdef = frame ? sem_find_function(vm->sem, frame->func) : NULL;
    int nGlobals = vm->sem->globals.count;

    SymRec **localRecs = NULL;
    int nLocals = 0, localCap = 0;
    if (fdef) {
        for (int i = 0; i < fdef->frame.count; i++) {
            SymRec *it = &fdef->frame.items[i];
            if (it->isParam) continue;
            int already = 0;
            for (int k = 0; k < nLocals; k++) if (strcmp(localRecs[k]->name, it->name) == 0) { already = 1; break; }
            if (already) continue;
            SymRec *last = it;
            for (int j = fdef->frame.count - 1; j >= 0; j--) {
                if (strcmp(fdef->frame.items[j].name, it->name) == 0) { last = &fdef->frame.items[j]; break; }
            }
            if (nLocals >= localCap) {
                localCap = localCap ? localCap * 2 : 16;
                localRecs = (SymRec **)xrealloc(localRecs, sizeof(SymRec *) * (size_t)localCap);
            }
            localRecs[nLocals++] = last;
        }
    }

    step->var_count = nGlobals + nLocals;
    step->variables = xmalloc(sizeof(*step->variables) * (step->var_count > 0 ? (size_t)step->var_count : 1));
    int vi = 0;
    for (int i = 0; i < nGlobals; i++) {
        strncpy(step->variables[vi].name, vm->sem->globals.items[i].name, sizeof(step->variables[vi].name) - 1);
        step->variables[vi].value = vm->mem[vm->sem->globals.items[i].offset];
        vi++;
    }
    for (int k = 0; k < nLocals; k++) {
        strncpy(step->variables[vi].name, localRecs[k]->name, sizeof(step->variables[vi].name) - 1);
        step->variables[vi].value = vm->mem[frame->bp + localRecs[k]->offset];
        vi++;
    }
    free(localRecs);

    step->frame_count = vm->frame_count;
    step->frames = xmalloc(sizeof(*step->frames) * (vm->frame_count > 0 ? (size_t)vm->frame_count : 1));
    for (int i = 0; i < vm->frame_count; i++) {
        snprintf(step->frames[i].func, sizeof(step->frames[i].func), "%s()", vm->frames[i].func);
        int rpc = vm->frames[i].retPC > 0 ? vm->frames[i].retPC : 0;
        snprintf(step->frames[i].retAddr, sizeof(step->frames[i].retAddr), "0x%04X", (unsigned)rpc);
    }

    step->console = xstrdup(vm->console ? vm->console : "");
}

/* --------------------------------- run ------------------------------------ */

VMResult *nova_vm_run(BcResult *bc, SemResult *sem, const char **inputs, int input_count) {
    VMResult *r = (VMResult *)xcalloc(1, sizeof(VMResult));
    r->runtimeDiags = diag_list_new();
    r->consoleOutput = xstrdup("");

    VM vm; memset(&vm, 0, sizeof(vm));
    vm.mem = (double *)xcalloc(VM_MEM_MAX, sizeof(double));
    vm.stack = (Cell *)xmalloc(sizeof(Cell) * VM_STACK_MAX);
    vm.frames = (Frame *)xmalloc(sizeof(Frame) * (VM_CALL_DEPTH_MAX + 1));
    vm.console = xstrdup("");
    vm.console_cap = 1;
    vm.randState = 1;
    vm.inputs = inputs;
    vm.input_count = input_count;
    vm.runtimeDiags = r->runtimeDiags;
    vm.result = r;
    vm.bc = bc;
    vm.sem = sem;

    int mainPC = bc_func_pc(bc, "main");
    if (mainPC < 0) {
        vm_runtime_error(&vm, "No 'main' entry point in bytecode", 0);
        r->consoleOutput = xstrdup(vm.console);
        free(vm.mem); free(vm.stack); free(vm.frames); free(vm.console);
        return r;
    }

    /* intern string literals into memory */
    vm.strBase_count = bc->string_count;
    vm.strBase = (int *)xmalloc(sizeof(int) * (bc->string_count > 0 ? (size_t)bc->string_count : 1));
    int cursor = sem->globalSlotCount;
    for (int si = 0; si < bc->string_count; si++) {
        vm.strBase[si] = cursor;
        const char *s = bc->strings[si];
        size_t slen = strlen(s);
        for (size_t k = 0; k < slen && cursor < VM_MEM_MAX; k++) vm.mem[cursor++] = (double)(unsigned char)s[k];
        if (cursor < VM_MEM_MAX) vm.mem[cursor++] = 0;
    }

    int mainBP = cursor;
    vm.frames[vm.frame_count].retPC = -1;
    strcpy(vm.frames[vm.frame_count].func, "main");
    vm.frames[vm.frame_count].bp = mainBP;
    vm.frame_count++;
    int memTop = mainBP + frame_size_of(&vm, "main");
    int pc = mainPC;
    long long stepsExecuted = 0;

    while (!vm.halted && pc < bc->count) {
        stepsExecuted++;
        if (stepsExecuted > VM_MAX_STEPS) {
            vm_runtime_error(&vm, "Execution step limit exceeded (possible infinite loop)", 0);
            break;
        }
        BcInstr *instr = &bc->items[pc];
        record_step(&vm, pc, instr);
        if (vm.halted) break;

        const char *op = instr->op;
        do {
        if (strcmp(op, "PUSH") == 0) { vm_push(&vm, instr->operand); pc++; }
        else if (strcmp(op, "PUSH_STR") == 0) {
            int idx = (int)instr->operand;
            vm_push(&vm, (idx >= 0 && idx < vm.strBase_count) ? (double)vm.strBase[idx] : 0.0);
            pc++;
        }
        else if (strcmp(op, "POP") == 0) { if (vm.sp > 0) vm.sp--; pc++; }
        else if (strcmp(op, "LOAD") == 0) {
            int addr = instr->isGlobal ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
            vm_push(&vm, vm.mem[addr]);
            pc++;
        }
        else if (strcmp(op, "STORE") == 0) {
            Cell v = vm_pop(&vm);
            int addr = instr->isGlobal ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
            vm.mem[addr] = v.n;
            pc++;
        }
        else if (strcmp(op, "ADDR") == 0) {
            int base = instr->isGlobal ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
            vm_push(&vm, (double)(base + (int)instr->operand));
            pc++;
        }
        else if (strcmp(op, "IDX_ADDR") == 0) {
            Cell idxCell = vm_pop(&vm);
            long long idx = truncate_ll(idxCell.n);
            int base = instr->isGlobal ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
            int size = (int)instr->operand;
            if (size >= 0 && (idx < 0 || idx >= size)) {
                char m[128]; snprintf(m, sizeof(m), "Array index %lld out of bounds (size %d)", idx, size);
                vm_runtime_error(&vm, m, instr->line);
                break;
            }
            vm_push(&vm, (double)(base + idx));
            pc++;
        }
        else if (strcmp(op, "LOAD_AT") == 0) {
            Cell addrCell = vm_pop(&vm);
            long long addr = truncate_ll(addrCell.n);
            if (addr < 0 || addr >= memTop) {
                char m[128]; snprintf(m, sizeof(m), "Invalid memory read at address %lld", addr);
                vm_runtime_error(&vm, m, instr->line);
                break;
            }
            vm_push(&vm, vm.mem[addr]);
            pc++;
        }
        else if (strcmp(op, "STORE_AT") == 0) {
            Cell addrCell = vm_pop(&vm);
            Cell valCell = vm_pop(&vm);
            long long addr = truncate_ll(addrCell.n);
            if (addr < 0 || addr >= memTop) {
                char m[128]; snprintf(m, sizeof(m), "Invalid memory write at address %lld", addr);
                vm_runtime_error(&vm, m, instr->line);
                break;
            }
            vm.mem[addr] = valCell.n;
            pc++;
        }
        else if (strcmp(op, "ADD") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n + b.n); pc++; }
        else if (strcmp(op, "SUB") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n - b.n); pc++; }
        else if (strcmp(op, "MUL") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n * b.n); pc++; }
        else if (strcmp(op, "DIV") == 0) {
            Cell b = vm_pop(&vm), a = vm_pop(&vm);
            long long bi = truncate_ll(b.n);
            if (bi == 0) { vm_runtime_error(&vm, "Division by zero", instr->line); break; }
            vm_push(&vm, (double)(truncate_ll(a.n) / bi));
            pc++;
        }
        else if (strcmp(op, "DIVF") == 0) {
            Cell b = vm_pop(&vm), a = vm_pop(&vm);
            if (b.n == 0) { vm_runtime_error(&vm, "Division by zero", instr->line); break; }
            vm_push(&vm, a.n / b.n);
            pc++;
        }
        else if (strcmp(op, "MOD") == 0) {
            Cell b = vm_pop(&vm), a = vm_pop(&vm);
            long long bi = truncate_ll(b.n);
            if (bi == 0) { vm_runtime_error(&vm, "Division by zero (modulo)", instr->line); break; }
            vm_push(&vm, (double)(truncate_ll(a.n) % bi));
            pc++;
        }
        else if (strcmp(op, "NEG") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, -a.n); pc++; }
        else if (strcmp(op, "NOT") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, a.n == 0 ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "EQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n == b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "NEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n != b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "LT") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n < b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "GT") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n > b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "LEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n <= b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "GEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n >= b.n ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "AND") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a.n != 0 && b.n != 0) ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "OR") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a.n != 0 || b.n != 0) ? 1.0 : 0.0); pc++; }
        else if (strcmp(op, "BAND") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (double)((int)truncate_ll(a.n) & (int)truncate_ll(b.n))); pc++; }
        else if (strcmp(op, "BOR") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (double)((int)truncate_ll(a.n) | (int)truncate_ll(b.n))); pc++; }
        else if (strcmp(op, "BXOR") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (double)((int)truncate_ll(a.n) ^ (int)truncate_ll(b.n))); pc++; }
        else if (strcmp(op, "BNOT") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, (double)(~(int)truncate_ll(a.n))); pc++; }
        else if (strcmp(op, "SHL") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (double)((int)truncate_ll(a.n) << ((int)truncate_ll(b.n) & 31))); pc++; }
        else if (strcmp(op, "SHR") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (double)((int)truncate_ll(a.n) >> ((int)truncate_ll(b.n) & 31))); pc++; }
        else if (strcmp(op, "CVT_I") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, (double)truncate_ll(a.n)); pc++; }
        else if (strcmp(op, "CVT_F") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, a.n); pc++; }
        else if (strcmp(op, "JMP") == 0) { pc = (int)instr->operand; }
        else if (strcmp(op, "JZ") == 0) { Cell v = vm_pop(&vm); pc = (v.n == 0) ? (int)instr->operand : pc + 1; }
        else if (strcmp(op, "CALL") == 0) {
            int target = bc_func_pc(bc, instr->symbol);
            if (target < 0) {
                if (call_builtin(&vm, instr)) { pc++; break; }
                char m[256]; snprintf(m, sizeof(m), "Call to undefined function '%s'", instr->symbol);
                vm_runtime_error(&vm, m, instr->line);
                break;
            }
            if (vm.frame_count >= VM_CALL_DEPTH_MAX) { vm_runtime_error(&vm, "Call stack overflow (recursion too deep)", instr->line); break; }
            int nargs = (int)instr->operand;
            if (vm.sp < nargs) { vm_runtime_error(&vm, "Stack underflow in CALL", instr->line); break; }
            int bp = memTop;
            int fsize = frame_size_of(&vm, instr->symbol);
            if (bp + fsize >= VM_MEM_MAX) { vm_runtime_error(&vm, "Memory exhausted (too many locals/frames)", instr->line); break; }
            for (int i = 0; i < nargs; i++) vm.mem[bp + i] = vm.stack[vm.sp - nargs + i].n;
            vm.sp -= nargs;
            strcpy(vm.frames[vm.frame_count].func, instr->symbol);
            vm.frames[vm.frame_count].retPC = pc + 1;
            vm.frames[vm.frame_count].bp = bp;
            vm.frame_count++;
            memTop = bp + fsize;
            pc = target;
        }
        else if (strcmp(op, "RET") == 0) {
            int hasVal = ((int)instr->operand == 1);
            Cell retVal; retVal.n = 0; retVal.s = 0; retVal.is_str = 0;
            if (hasVal) retVal = vm_pop(&vm);
            if (vm.frame_count == 0) { vm_runtime_error(&vm, "RET with empty call stack", instr->line); break; }
            vm.frame_count--;
            Frame frame = vm.frames[vm.frame_count];
            memTop = frame.bp;
            if (vm.frame_count == 0) {
                vm.exitCode = (int)truncate_ll(retVal.n);
                vm.halted = 1;
                break;
            }
            vm_push(&vm, retVal.n);
            pc = frame.retPC;
        }
        else if (strcmp(op, "PRINT") == 0) {
            int nargs = (int)instr->operand;
            if (vm.sp < nargs) { vm_runtime_error(&vm, "Stack underflow in PRINT", instr->line); break; }
            Cell *args = (Cell *)xmalloc(sizeof(Cell) * (nargs > 0 ? (size_t)nargs : 1));
            for (int i = 0; i < nargs; i++) args[i] = vm.stack[vm.sp - nargs + i];
            vm.sp -= nargs;
            const char *fmt = (instr->fmtIdx >= 0 && instr->fmtIdx < bc->string_count) ? bc->strings[instr->fmtIdx] : "";
            format_printf(&vm, fmt, args, nargs);
            free(args);
            pc++;
        }
        else if (strcmp(op, "INPUT") == 0) {
            int nTargets = (int)instr->operand;
            if (vm.sp < nTargets) { vm_runtime_error(&vm, "Stack underflow in INPUT", instr->line); break; }
            if (vm.inputIdx + nTargets > vm.input_count) {
                vm.waitingForInput = 1;
                const char *fmt = (instr->fmtIdx >= 0 && instr->fmtIdx < bc->string_count) ? bc->strings[instr->fmtIdx] : "%d";
                snprintf(vm.inputPrompt, sizeof(vm.inputPrompt), "Enter %d value(s) for scanf (%s)", nTargets, fmt);
                vm.halted = 1;
                break;
            }
            Cell addrs[16];
            int na = nTargets > 16 ? 16 : nTargets;
            for (int i = 0; i < na; i++) addrs[i] = vm.stack[vm.sp - nTargets + i];
            vm.sp -= nTargets;
            const char *fmt = (instr->fmtIdx >= 0 && instr->fmtIdx < bc->string_count) ? bc->strings[instr->fmtIdx] : "%d";
            int fpos = 0;
            for (int i = 0; i < na; i++) {
                char conv = scanf_next_conv(fmt, &fpos);
                const char *raw = vm.inputs[vm.inputIdx++];
                long long addr = truncate_ll(addrs[i].n);
                if (addr < 0 || addr >= memTop) {
                    char m[128]; snprintf(m, sizeof(m), "Invalid scanf target address %lld", addr);
                    vm_runtime_error(&vm, m, instr->line);
                    break;
                }
                char buf[256];
                strncpy(buf, raw, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
                /* trim leading whitespace */
                char *s = buf; while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                if (conv == 'c') {
                    /* %c reads the first character of the next input line */
                    vm.mem[addr] = *s ? (double)(unsigned char)*s : 0.0;
                } else if (conv == 's') {
                    /* %s copies the input token (the line, trimmed) plus NUL */
                    long long cap = memTop - addr;
                    if (cap > 4096) cap = 4096;
                    if (cap < 1) cap = 1;
                    long long k = 0;
                    while (s[k] && k < cap - 1) { vm.mem[addr + k] = (double)(unsigned char)s[k]; k++; }
                    vm.mem[addr + k] = 0;
                } else {
                    char *endp = NULL;
                    double val = strtod(s, &endp);
                    if (endp == s) val = 0;
                    if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o')
                        val = (double)truncate_ll(val);
                    vm.mem[addr] = val;
                }
            }
            if (vm.halted) break;
            pc++;
        }
        else if (strcmp(op, "HALT") == 0) { vm.halted = 1; }
        else { pc++; }
        } while (0);
    }

    /* finalize result */
    r->exitCode = vm.exitCode;
    r->waitingForInput = vm.waitingForInput;
    strncpy(r->inputPrompt, vm.inputPrompt, sizeof(r->inputPrompt) - 1);
    free(r->consoleOutput);
    r->consoleOutput = xstrdup(vm.console);

    free(vm.strBase);
    free(vm.mem);
    free(vm.stack);
    free(vm.frames);
    free(vm.console);
    return r;
}

void nova_vm_free(VMResult *v) {
    if (!v) return;
    for (int i = 0; i < v->count; i++) {
        free(v->steps[i].stack);
        free(v->steps[i].variables);
        free(v->steps[i].frames);
        free(v->steps[i].console);
    }
    free(v->steps);
    free(v->consoleOutput);
    diag_list_free(v->runtimeDiags);
    free(v);
}

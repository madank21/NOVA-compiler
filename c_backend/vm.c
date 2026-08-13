#include "vm.h"
#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void* xmalloc(size_t n) { void* p = malloc(n); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static void* xrealloc(void* p, size_t n) { void* q = realloc(p, n); if (!q) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return q; }
static void* xcalloc(size_t n, size_t s) { void* p = calloc(n, s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }
static char* xstrdup(const char* s) { char* p = strdup(s); if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); } return p; }

/* ------------------------------------------------------------------------- */
/* Growable console buffer                                                    */
/* ------------------------------------------------------------------------- */

typedef struct { char* data; int len; int capacity; } Console;

static void console_init(Console* c) {
    c->capacity = 256;
    c->len = 0;
    c->data = (char*)xmalloc((size_t)c->capacity);
    c->data[0] = '\0';
}

static void console_append(Console* c, const char* s) {
    int n = (int)strlen(s);
    while (c->len + n + 1 >= c->capacity) {
        c->capacity *= 2;
        c->data = (char*)xrealloc(c->data, (size_t)c->capacity);
    }
    memcpy(c->data + c->len, s, (size_t)n);
    c->len += n;
    c->data[c->len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Stack cells (number or string-pool reference)                              */
/* ------------------------------------------------------------------------- */

typedef struct { double n; int s; int is_str; } Cell;

typedef struct { char func[256]; int ret_pc; int bp; } Frame;

typedef struct {
    const BytecodeChunk* chunk;
    SemModel* sem;
    DiagList* diags;
    double* mem;
    int mem_top;
    Cell* stack;
    int sp;
    Frame* frames;
    int frame_count;
    Console console;
    VMResult* result;
    int halted;
    int input_idx;
    const char** inputs;
    int input_count;
    /* snapshot metadata */
    SymRec* global_list;   /* == sem->globals.items */
    int global_count;
} VM;

static void vm_runtime_error(VM* vm, const char* msg, int line) {
    diag_add(vm->diags, "runtime", line, 0, "%s", msg);
    vm->halted = 1;
}

static void vm_push(VM* vm, double v) {
    if (vm->sp >= VM_STACK_MAX) { vm_runtime_error(vm, "Operand stack overflow", 0); return; }
    vm->stack[vm->sp].n = v;
    vm->stack[vm->sp].is_str = 0;
    vm->sp++;
}

static void vm_push_str(VM* vm, int idx) {
    if (vm->sp >= VM_STACK_MAX) { vm_runtime_error(vm, "Operand stack overflow", 0); return; }
    vm->stack[vm->sp].s = idx;
    vm->stack[vm->sp].n = 0;
    vm->stack[vm->sp].is_str = 1;
    vm->sp++;
}

static Cell vm_pop(VM* vm) {
    if (vm->sp > 0) { vm->sp--; return vm->stack[vm->sp]; }
    vm_runtime_error(vm, "Operand stack underflow", 0);
    Cell c; c.n = 0; c.s = 0; c.is_str = 0;
    return c;
}

static int frame_size_of(VM* vm, const char* func) {
    FuncDef* f = sem_get_function(vm->sem, func);
    int base = f ? f->frame_size : 0;
    return base + chunk_temps_for(vm->chunk, func);
}

static void describe_instr(const BInstr* i, char* out, size_t out_size) {
    char num[64];
    if (strcmp(i->op, "PUSH") == 0) {
        format_value(i->operand, num, sizeof(num));
        snprintf(out, out_size, "PUSH %s", num);
    } else if (strcmp(i->op, "LOAD") == 0) {
        snprintf(out, out_size, "LOAD %s", i->symbol);
    } else if (strcmp(i->op, "STORE") == 0) {
        snprintf(out, out_size, "STORE %s", i->symbol);
    } else if (strcmp(i->op, "ADDR") == 0) {
        format_value(i->operand, num, sizeof(num));
        snprintf(out, out_size, "ADDR %s+%s", i->symbol, num);
    } else if (strcmp(i->op, "JMP") == 0) {
        snprintf(out, out_size, "JMP %s", i->symbol);
    } else if (strcmp(i->op, "JZ") == 0) {
        snprintf(out, out_size, "JZ %s", i->symbol);
    } else if (strcmp(i->op, "CALL") == 0) {
        snprintf(out, out_size, "CALL %s (%lld args)", i->symbol, nova_truncate_i64(i->operand));
    } else if (strcmp(i->op, "RET") == 0) {
        snprintf(out, out_size, "RET");
    } else if (strcmp(i->op, "PRINT") == 0) {
        snprintf(out, out_size, "PRINT");
    } else if (strcmp(i->op, "INPUT") == 0) {
        snprintf(out, out_size, "INPUT");
    } else if (strcmp(i->op, "HALT") == 0) {
        snprintf(out, out_size, "HALT");
    } else {
        snprintf(out, out_size, "%s", i->op);
    }
}

static void record_step(VM* vm, int pc, const BInstr* instr) {
    VMResult* r = vm->result;
    if (r->count >= VM_TRACE_MAX_STEPS) { r->truncated = 1; return; }
    if (r->count >= r->capacity) {
        r->capacity = r->capacity ? r->capacity * 2 : 256;
        r->steps = (VMStep*)xrealloc(r->steps, sizeof(VMStep) * (size_t)r->capacity);
    }
    VMStep* step = &r->steps[r->count++];
    memset(step, 0, sizeof(VMStep));
    step->step = r->count - 1;
    step->pc = pc;
    step->line = instr->line;
    char ibuf[192];
    describe_instr(instr, ibuf, sizeof(ibuf));
    step->instruction = xstrdup(ibuf);

    step->stack_count = vm->sp;
    step->stack = (double*)xmalloc(sizeof(double) * (size_t)(vm->sp > 0 ? vm->sp : 1));
    for (int i = 0; i < vm->sp; i++) {
        step->stack[i] = vm->stack[i].is_str ? 0.0 : vm->stack[i].n;
    }

    int var_count = vm->global_count;
    Frame* frame = vm->frame_count > 0 ? &vm->frames[vm->frame_count - 1] : NULL;
    FuncDef* fdef = frame ? sem_get_function(vm->sem, frame->func) : NULL;

    /* Mirror the JS engine's Map<name,rec>: each local name appears once,
     * at its first-insertion position, carrying its latest rec. Build a
     * deduplicated index list of frame entries. */
    int* local_idx = NULL;
    int local_count = 0;
    if (fdef) {
        local_idx = (int*)xmalloc(sizeof(int) * (size_t)(fdef->frame.count > 0 ? fdef->frame.count : 1));
        for (int i = 0; i < fdef->frame.count; i++) {
            SymRec* rec = &fdef->frame.items[i];
            if (rec->is_param) continue;
            int already = 0;
            for (int k = 0; k < local_count; k++) {
                if (strcmp(fdef->frame.items[local_idx[k]].name, rec->name) == 0) { already = 1; break; }
            }
            if (!already) local_idx[local_count++] = i;
        }
        /* point each kept slot at the LATEST rec with that name */
        for (int k = 0; k < local_count; k++) {
            const char* nm = fdef->frame.items[local_idx[k]].name;
            for (int j = local_idx[k] + 1; j < fdef->frame.count; j++) {
                if (strcmp(fdef->frame.items[j].name, nm) == 0) local_idx[k] = j;
            }
        }
    }

    step->var_count = var_count + local_count;
    step->variables = (VMVarSnap*)xmalloc(sizeof(VMVarSnap) * (size_t)(step->var_count > 0 ? step->var_count : 1));
    int vi = 0;
    for (int i = 0; i < vm->global_count; i++) {
        snprintf(step->variables[vi].name, sizeof(step->variables[0].name), "%s", vm->global_list[i].name);
        step->variables[vi].value = vm->mem[vm->global_list[i].offset];
        vi++;
    }
    if (fdef) {
        for (int k = 0; k < local_count; k++) {
            SymRec* rec = &fdef->frame.items[local_idx[k]];
            snprintf(step->variables[vi].name, sizeof(step->variables[0].name), "%s", rec->name);
            step->variables[vi].value = vm->mem[frame->bp + rec->offset];
            vi++;
        }
    }
    free(local_idx);

    step->frame_count = vm->frame_count;
    step->frames = (VMFrameSnap*)xmalloc(sizeof(VMFrameSnap) * (size_t)(vm->frame_count > 0 ? vm->frame_count : 1));
    for (int i = 0; i < vm->frame_count; i++) {
        snprintf(step->frames[i].func, sizeof(step->frames[0].func), "%s()", vm->frames[i].func);
        int rpc = vm->frames[i].ret_pc > 0 ? vm->frames[i].ret_pc : 0;
        snprintf(step->frames[i].ret_addr, sizeof(step->frames[0].ret_addr), "0x%04X", (unsigned)rpc);
    }

    step->console = xstrdup(vm->console.data);
}

/* printf formatting (mirrors the JS formatPrintf exactly) */
static void format_printf(VM* vm, const char* fmt, Cell* args, int nargs, Console* out) {
    char buf[128];
    int ai = 0;
    size_t len = strlen(fmt);
    for (size_t i = 0; i < len; i++) {
        char ch = fmt[i];
        if (ch != '%') {
            char one[2] = { ch, '\0' };
            console_append(out, one);
            continue;
        }
        i++;
        if (i < len && fmt[i] == '%') { console_append(out, "%"); continue; }
        int prec = -1;
        if (i < len && fmt[i] == '.') {
            i++;
            char digits[16];
            int dn = 0;
            while (i < len && fmt[i] >= '0' && fmt[i] <= '9' && dn < 15) {
                digits[dn++] = fmt[i++];
            }
            digits[dn] = '\0';
            prec = dn == 0 ? 0 : atoi(digits);
        }
        while (i < len && (fmt[i] == 'l' || fmt[i] == 'h')) i++;
        char conv = i < len ? fmt[i] : '\0';
        Cell arg;
        if (ai < nargs) arg = args[ai++];
        else { arg.n = 0; arg.s = 0; arg.is_str = 0; }
        double val = arg.is_str ? 0.0 : arg.n;
        if (conv == 'd' || conv == 'i') {
            snprintf(buf, sizeof(buf), "%lld", nova_truncate_i64(val));
            console_append(out, buf);
        } else if (conv == 'f') {
            snprintf(buf, sizeof(buf), "%.*f", prec == -1 ? 6 : prec, val);
            console_append(out, buf);
        } else if (conv == 'c') {
            char cbuf[2] = { (char)(nova_truncate_i64(val) & 0xff), '\0' };
            console_append(out, cbuf);
        } else if (conv == 's') {
            if (arg.is_str && vm->chunk->strings && arg.s >= 0 && arg.s < vm->chunk->strings->count) {
                console_append(out, vm->chunk->strings->items[arg.s]);
            } else {
                snprintf(buf, sizeof(buf), "%lld", nova_truncate_i64(val));
                console_append(out, buf);
            }
        } else {
            char ubuf[3] = { '%', conv, '\0' };
            console_append(out, ubuf);
        }
    }
}

VMResult* vm_execute(const BytecodeChunk* chunk, SemModel* sem,
                     const char** inputs, int input_count, DiagList* diags) {
    VMResult* r = (VMResult*)xcalloc(1, sizeof(VMResult));
    r->runtime_diags = diags;

    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm.chunk = chunk;
    vm.sem = sem;
    vm.diags = diags;
    vm.result = r;
    vm.inputs = inputs;
    vm.input_count = input_count;
    vm.mem = (double*)xcalloc(VM_MEM_MAX, sizeof(double));
    vm.stack = (Cell*)xmalloc(sizeof(Cell) * VM_STACK_MAX);
    vm.frames = (Frame*)xmalloc(sizeof(Frame) * (VM_CALL_DEPTH_MAX + 1));
    console_init(&vm.console);
    vm.global_list = sem->globals.items;
    vm.global_count = sem->globals.count;

    int main_pc = chunk_func_pc(chunk, "main");
    if (main_pc < 0) {
        vm_runtime_error(&vm, "No 'main' entry point in bytecode", 0);
        goto done;
    }

    {
        int main_frame_size = frame_size_of(&vm, "main");
        Frame* f = &vm.frames[vm.frame_count++];
        strcpy(f->func, "main");
        f->ret_pc = -1;
        f->bp = sem->global_slot_count;
        vm.mem_top = sem->global_slot_count + main_frame_size;
    }

    {
        int pc = main_pc;
        long long steps_executed = 0;

        while (!vm.halted && pc < chunk->count) {
            steps_executed++;
            if (steps_executed > VM_MAX_STEPS) {
                vm_runtime_error(&vm, "Execution step limit exceeded (possible infinite loop)", 0);
                break;
            }
            const BInstr* instr = &chunk->code[pc];
            record_step(&vm, pc, instr);
            if (vm.halted) break;

            if (strcmp(instr->op, "PUSH") == 0) { vm_push(&vm, instr->operand); pc++; }
            else if (strcmp(instr->op, "PUSH_STR") == 0) { vm_push_str(&vm, (int)instr->operand); pc++; }
            else if (strcmp(instr->op, "POP") == 0) { if (vm.sp > 0) vm.sp--; pc++; }
            else if (strcmp(instr->op, "LOAD") == 0) {
                int addr = instr->is_global ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
                vm_push(&vm, vm.mem[addr]);
                pc++;
            }
            else if (strcmp(instr->op, "STORE") == 0) {
                Cell v = vm_pop(&vm);
                int addr = instr->is_global ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
                vm.mem[addr] = v.n;
                pc++;
            }
            else if (strcmp(instr->op, "ADDR") == 0) {
                int base = instr->is_global ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
                vm_push(&vm, (double)(base + (int)instr->operand));
                pc++;
            }
            else if (strcmp(instr->op, "IDX_ADDR") == 0) {
                Cell idx_cell = vm_pop(&vm);
                long long idx = nova_truncate_i64(idx_cell.n);
                int base = instr->is_global ? instr->slot : vm.frames[vm.frame_count - 1].bp + instr->slot;
                if (instr->array_size >= 0 && (idx < 0 || idx >= instr->array_size)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Array index %lld out of bounds (size %d)", idx, instr->array_size);
                    vm_runtime_error(&vm, msg, instr->line);
                    break;
                }
                vm_push(&vm, (double)(base + idx));
                pc++;
            }
            else if (strcmp(instr->op, "LOAD_AT") == 0) {
                Cell addr_cell = vm_pop(&vm);
                long long addr = nova_truncate_i64(addr_cell.n);
                if (addr < 0 || addr >= vm.mem_top) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Invalid memory read at address %lld", addr);
                    vm_runtime_error(&vm, msg, instr->line);
                    break;
                }
                vm_push(&vm, vm.mem[addr]);
                pc++;
            }
            else if (strcmp(instr->op, "STORE_AT") == 0) {
                Cell addr_cell = vm_pop(&vm);
                Cell val_cell = vm_pop(&vm);
                long long addr = nova_truncate_i64(addr_cell.n);
                if (addr < 0 || addr >= vm.mem_top) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Invalid memory write at address %lld", addr);
                    vm_runtime_error(&vm, msg, instr->line);
                    break;
                }
                vm.mem[addr] = val_cell.n;
                pc++;
            }
            else if (strcmp(instr->op, "ADD") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n + b.n); pc++; }
            else if (strcmp(instr->op, "SUB") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n - b.n); pc++; }
            else if (strcmp(instr->op, "MUL") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n * b.n); pc++; }
            else if (strcmp(instr->op, "DIV") == 0) {
                Cell b = vm_pop(&vm), a = vm_pop(&vm);
                long long bi = nova_truncate_i64(b.n);
                if (bi == 0) { vm_runtime_error(&vm, "Division by zero", instr->line); break; }
                long long ai = nova_truncate_i64(a.n);
                vm_push(&vm, (double)(ai / bi));
                pc++;
            }
            else if (strcmp(instr->op, "DIVF") == 0) {
                Cell b = vm_pop(&vm), a = vm_pop(&vm);
                if (b.n == 0.0) { vm_runtime_error(&vm, "Division by zero", instr->line); break; }
                vm_push(&vm, a.n / b.n);
                pc++;
            }
            else if (strcmp(instr->op, "MOD") == 0) {
                Cell b = vm_pop(&vm), a = vm_pop(&vm);
                long long bi = nova_truncate_i64(b.n);
                if (bi == 0) { vm_runtime_error(&vm, "Division by zero (modulo)", instr->line); break; }
                long long ai = nova_truncate_i64(a.n);
                vm_push(&vm, (double)(ai % bi));
                pc++;
            }
            else if (strcmp(instr->op, "NEG") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, -a.n); pc++; }
            else if (strcmp(instr->op, "NOT") == 0) { Cell a = vm_pop(&vm); vm_push(&vm, a.n == 0.0 ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "EQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n == b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "NEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n != b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "LT") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n < b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "GT") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n > b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "LEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n <= b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "GEQ") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a.n >= b.n ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "AND") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a.n != 0.0 && b.n != 0.0) ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "OR") == 0) { Cell b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a.n != 0.0 || b.n != 0.0) ? 1.0 : 0.0); pc++; }
            else if (strcmp(instr->op, "JMP") == 0) { pc = (int)instr->operand; }
            else if (strcmp(instr->op, "JZ") == 0) {
                Cell v = vm_pop(&vm);
                pc = (v.n == 0.0) ? (int)instr->operand : pc + 1;
            }
            else if (strcmp(instr->op, "CALL") == 0) {
                int target = chunk_func_pc(chunk, instr->symbol);
                if (target < 0) {
                    char msg[300];
                    snprintf(msg, sizeof(msg), "Call to undefined function '%s'", instr->symbol);
                    vm_runtime_error(&vm, msg, instr->line);
                    break;
                }
                if (vm.frame_count >= VM_CALL_DEPTH_MAX) {
                    vm_runtime_error(&vm, "Call stack overflow (recursion too deep)", instr->line);
                    break;
                }
                int nargs = (int)instr->operand;
                if (vm.sp < nargs) { vm_runtime_error(&vm, "Stack underflow in CALL", instr->line); break; }
                int bp = vm.mem_top;
                int fsize = frame_size_of(&vm, instr->symbol);
                if (bp + fsize >= VM_MEM_MAX) {
                    vm_runtime_error(&vm, "Memory exhausted (too many locals/frames)", instr->line);
                    break;
                }
                for (int i = 0; i < nargs; i++) {
                    Cell cell = vm.stack[vm.sp - nargs + i];
                    vm.mem[bp + i] = cell.n;
                }
                vm.sp -= nargs;
                Frame* f = &vm.frames[vm.frame_count++];
                snprintf(f->func, sizeof(f->func), "%s", instr->symbol);
                f->func[sizeof(f->func) - 1] = '\0';
                f->ret_pc = pc + 1;
                f->bp = bp;
                vm.mem_top = bp + fsize;
                pc = target;
            }
            else if (strcmp(instr->op, "RET") == 0) {
                int has_val = instr->operand == 1.0;
                Cell ret_val;
                ret_val.n = 0; ret_val.s = 0; ret_val.is_str = 0;
                if (has_val) ret_val = vm_pop(&vm);
                if (vm.frame_count == 0) { vm_runtime_error(&vm, "RET with empty call stack", instr->line); break; }
                Frame frame = vm.frames[--vm.frame_count];
                vm.mem_top = frame.bp;
                if (vm.frame_count == 0) {
                    r->exit_code = (int)nova_truncate_i64(ret_val.n);
                    vm.halted = 1;
                    break;
                }
                vm_push(&vm, ret_val.n);
                pc = frame.ret_pc;
            }
            else if (strcmp(instr->op, "PRINT") == 0) {
                int nargs = (int)instr->operand;
                if (vm.sp < nargs) { vm_runtime_error(&vm, "Stack underflow in PRINT", instr->line); break; }
                Cell* args = (Cell*)xmalloc(sizeof(Cell) * (size_t)(nargs > 0 ? nargs : 1));
                for (int i = 0; i < nargs; i++) args[i] = vm.stack[vm.sp - nargs + i];
                vm.sp -= nargs;
                const char* fmt = (instr->fmt_idx >= 0 && chunk->strings && instr->fmt_idx < chunk->strings->count)
                    ? chunk->strings->items[instr->fmt_idx] : "";
                format_printf(&vm, fmt, args, nargs, &vm.console);
                free(args);
                pc++;
            }
            else if (strcmp(instr->op, "INPUT") == 0) {
                int ntargets = (int)instr->operand;
                if (vm.sp < ntargets) { vm_runtime_error(&vm, "Stack underflow in INPUT", instr->line); break; }
                if (vm.input_idx + ntargets > vm.input_count) {
                    r->waiting_for_input = 1;
                    const char* fmt = (instr->fmt_idx >= 0 && chunk->strings && instr->fmt_idx < chunk->strings->count)
                        ? chunk->strings->items[instr->fmt_idx] : "%d";
                    snprintf(r->input_prompt, sizeof(r->input_prompt),
                             "Enter %d value(s) for scanf (%s)", ntargets, fmt);
                    vm.halted = 1;
                    break;
                }
                Cell addrs[16];
                int take = ntargets > 16 ? 16 : ntargets;
                for (int i = 0; i < take; i++) addrs[i] = vm.stack[vm.sp - ntargets + i];
                vm.sp -= ntargets;
                const char* fmt = (instr->fmt_idx >= 0 && chunk->strings && instr->fmt_idx < chunk->strings->count)
                    ? chunk->strings->items[instr->fmt_idx] : "%d";
                for (int i = 0; i < take; i++) {
                    /* trim */
                    const char* raw = vm.inputs[vm.input_idx++];
                    char buf[256];
                    int si = 0;
                    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;
                    while (raw[si] && si < 255) { buf[si] = raw[si]; si++; }
                    buf[si] = '\0';
                    while (si > 0 && (buf[si - 1] == ' ' || buf[si - 1] == '\t' || buf[si - 1] == '\n' || buf[si - 1] == '\r')) buf[--si] = '\0';
                    char* endp = NULL;
                    double val = strtod(buf, &endp);
                    if (endp == buf) val = 0; /* NaN -> 0, mirrors JS */
                    if (strstr(fmt, "%d") && i == 0) val = (double)nova_truncate_i64(val);
                    long long addr = nova_truncate_i64(addrs[i].n);
                    if (addr < 0 || addr >= vm.mem_top) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Invalid scanf target address %lld", addr);
                        vm_runtime_error(&vm, msg, instr->line);
                        break;
                    }
                    vm.mem[addr] = val;
                }
                if (vm.halted) break;
                pc++;
            }
            else if (strcmp(instr->op, "HALT") == 0) { vm.halted = 1; }
            else { pc++; }
        }
    }

done:
    r->console_output = xstrdup(vm.console.data);
    free(vm.mem);
    free(vm.stack);
    free(vm.frames);
    free(vm.console.data);
    return r;
}

void vm_result_free(VMResult* r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->steps[i].instruction);
        free(r->steps[i].stack);
        free(r->steps[i].variables);
        free(r->steps[i].frames);
        free(r->steps[i].console);
    }
    free(r->steps);
    free(r->console_output);
    free(r);
}
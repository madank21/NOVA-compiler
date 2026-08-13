#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------- growable output buffer ------------------------ */

typedef struct { char *data; int len, cap; } Buf;

static void buf_init(Buf *b) { b->cap = 8192; b->len = 0; b->data = (char *)malloc((size_t)b->cap); if (!b->data) { fprintf(stderr, "nova: out of memory\n"); exit(1); } b->data[0] = '\0'; }
static void buf_reserve(Buf *b, int extra) {
    while (b->len + extra + 1 >= b->cap) {
        b->cap *= 2;
        char *g = (char *)realloc(b->data, (size_t)b->cap);
        if (!g) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        b->data = g;
    }
}
static void buf_append(Buf *b, const char *s) {
    int len = (int)strlen(s);
    buf_reserve(b, len);
    memcpy(b->data + b->len, s, (size_t)len);
    b->len += len;
    b->data[b->len] = '\0';
}
static void buf_append_n(Buf *b, const char *s, int n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* JSON number formatting that matches JS JSON.stringify (shortest round-trip). */
static void buf_append_number(Buf *b, double v) {
    if (!isfinite(v)) { buf_append(b, "null"); return; }
    char tmp[64];
    nova_fmt_shortest(v, tmp, sizeof(tmp));
    buf_append(b, tmp);
}

/* JSON string escaping matching the JS engine. */
static void buf_append_escaped(Buf *b, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': buf_append(b, "\\\""); break;
            case '\\': buf_append(b, "\\\\"); break;
            case '\n': buf_append(b, "\\n"); break;
            case '\r': buf_append(b, "\\r"); break;
            case '\t': buf_append(b, "\\t"); break;
            default:
                if (*p < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    buf_append(b, tmp);
                } else {
                    buf_append_n(b, (const char *)p, 1);
                }
                break;
        }
    }
}

/* ------------------------------- sections -------------------------------- */

static void serialize_tokens(Buf *b, NovaTokenList *tokens) {
    buf_append(b, "[");
    for (int i = 0; i < tokens->count; i++) {
        if (i) buf_append(b, ",");
        NovaToken *t = &tokens->items[i];
        char tmp[64];
        buf_append(b, "{\"type\":\"");
        buf_append(b, nova_token_type_name(t->type));
        buf_append(b, "\",\"lexeme\":\"");
        buf_append_escaped(b, t->lexeme);
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d,\"column\":%d}", t->line, t->column);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_ast_node(Buf *b, NovaNode *node) {
    if (!node) { buf_append(b, "null"); return; }
    char tmp[64];
    buf_append(b, "{\"type\":\"");
    buf_append(b, nova_node_type_name(node->type));
    buf_append(b, "\"");
    if (node->has_identifier) {
        buf_append(b, ",\"identifier\":\"");
        buf_append_escaped(b, node->identifier);
        buf_append(b, "\"");
    }
    if (node->has_type_name) {
        buf_append(b, ",\"type_name\":\"");
        buf_append_escaped(b, node->type_name);
        buf_append(b, "\"");
    }
    if (node->has_op) {
        buf_append(b, ",\"op\":\"");
        buf_append_escaped(b, node->op);
        buf_append(b, "\"");
    }
    if (node->has_num) {
        buf_append(b, ",\"num_val\":");
        buf_append_number(b, node->num_val);
    }
    if (node->has_string) {
        buf_append(b, ",\"string_val\":\"");
        buf_append_escaped(b, node->string_val);
        buf_append(b, "\"");
    }
    if (node->is_array) buf_append(b, ",\"is_array\":true");
    if (node->has_size) buf_append(b, ",\"has_size\":true");
    if (node->is_static) buf_append(b, ",\"is_static\":true");
    snprintf(tmp, sizeof(tmp), ",\"line\":%d", node->line);
    buf_append(b, tmp);
    buf_append(b, ",\"children\":[");
    for (int i = 0; i < node->child_count; i++) {
        if (i) buf_append(b, ",");
        serialize_ast_node(b, node->children[i]);
    }
    buf_append(b, "]}");
}

static void serialize_symbol_table(Buf *b, SemResult *sem) {
    buf_append(b, "[");
    for (int i = 0; i < sem->symbol_count; i++) {
        if (i) buf_append(b, ",");
        SymRow *row = &sem->symbols[i];
        char tmp[64];
        buf_append(b, "{\"scope\":\"");
        buf_append_escaped(b, row->scope);
        buf_append(b, "\",\"name\":\"");
        buf_append_escaped(b, row->name);
        buf_append(b, "\",\"kind\":\"");
        buf_append_escaped(b, row->kind);
        buf_append(b, "\",\"type\":\"");
        buf_append_escaped(b, row->type);
        buf_append(b, "\",\"address\":\"");
        buf_append(b, row->address);
        snprintf(tmp, sizeof(tmp), "\",\"params\":%d}", row->params);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_tac(Buf *b, TacResult *tac) {
    buf_append(b, "[");
    for (int i = 0; tac && i < tac->count; i++) {
        if (i) buf_append(b, ",");
        TacInstr *ins = &tac->items[i];
        char tmp[64];
        buf_append(b, "{\"op\":\"");
        buf_append_escaped(b, ins->op);
        buf_append(b, "\",\"res\":\"");
        buf_append_escaped(b, ins->res);
        buf_append(b, "\",\"a1\":\"");
        buf_append_escaped(b, ins->a1);
        buf_append(b, "\",\"a2\":\"");
        buf_append_escaped(b, ins->a2);
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d}", ins->line);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_bytecode(Buf *b, BcResult *bc) {
    buf_append(b, "[");
    for (int i = 0; bc && i < bc->count; i++) {
        if (i) buf_append(b, ",");
        BcInstr *ins = &bc->items[i];
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "{\"pc\":%d,\"op\":\"", ins->pc);
        buf_append(b, tmp);
        buf_append_escaped(b, ins->op);
        buf_append(b, "\",\"operand\":");
        buf_append_number(b, ins->operand);
        buf_append(b, ",\"symbol\":\"");
        buf_append_escaped(b, ins->symbol);
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d}", ins->line);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_vm_trace(Buf *b, VMResult *vm) {
    buf_append(b, "[");
    for (int i = 0; vm && i < vm->count; i++) {
        if (i) buf_append(b, ",");
        VMStep *step = &vm->steps[i];
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "{\"step\":%d,\"pc\":%d,\"line\":%d,\"instruction\":\"", i, step->pc, step->line);
        buf_append(b, tmp);
        buf_append_escaped(b, step->instruction);
        buf_append(b, "\",\"stack\":[");
        for (int j = 0; j < step->stack_count; j++) {
            if (j) buf_append(b, ",");
            buf_append_number(b, step->stack[j]);
        }
        buf_append(b, "],\"variables\":[");
        for (int j = 0; j < step->var_count; j++) {
            if (j) buf_append(b, ",");
            buf_append(b, "{\"name\":\"");
            buf_append_escaped(b, step->variables[j].name);
            buf_append(b, "\",\"value\":");
            buf_append_number(b, step->variables[j].value);
            buf_append(b, "}");
        }
        buf_append(b, "],\"frames\":[");
        for (int j = 0; j < step->frame_count; j++) {
            if (j) buf_append(b, ",");
            buf_append(b, "{\"func\":\"");
            buf_append_escaped(b, step->frames[j].func);
            buf_append(b, "\",\"retAddr\":\"");
            buf_append(b, step->frames[j].retAddr);
            buf_append(b, "\"}");
        }
        buf_append(b, "],\"console\":\"");
        buf_append_escaped(b, step->console);
        buf_append(b, "\"}");
    }
    buf_append(b, "]");
}

static void serialize_diagnostics(Buf *b, DiagList *diags) {
    buf_append(b, "[");
    for (int i = 0; diags && i < diags->count; i++) {
        if (i) buf_append(b, ",");
        Diag *d = &diags->items[i];
        char tmp[64];
        buf_append(b, "{\"level\":\"");
        buf_append(b, d->level);
        buf_append(b, "\",\"msg\":\"");
        buf_append_escaped(b, d->msg);
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d,\"column\":%d}", d->line, d->column);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

char *nova_to_json(CompileResult *r) {
    Buf b;
    buf_init(&b);
    char tmp[128];

    buf_append(&b, "{\"success\":");
    buf_append(&b, r->success ? "true" : "false");
    buf_append(&b, ",\"engine\":\"");
    buf_append(&b, r->engine);
    buf_append(&b, "\",\"compile_time_ms\":");
    buf_append_number(&b, r->compile_time_ms);

    buf_append(&b, ",\"tokens\":");
    serialize_tokens(&b, r->tokens);

    buf_append(&b, ",\"ast\":");
    serialize_ast_node(&b, r->ast);

    buf_append(&b, ",\"symbolTable\":");
    if (r->sem) serialize_symbol_table(&b, r->sem); else buf_append(&b, "[]");

    buf_append(&b, ",\"tac\":");
    serialize_tac(&b, r->tac);

    buf_append(&b, ",\"optTac\":");
    serialize_tac(&b, r->optTac);

    snprintf(tmp, sizeof(tmp),
        ",\"metrics\":{\"constant_fold\":%d,\"constant_prop\":%d,\"dead_code\":%d,\"strength_reduce\":%d,\"reduction_percentage\":",
        r->metrics.constant_fold, r->metrics.constant_prop, r->metrics.dead_code, r->metrics.strength_reduce);
    buf_append(&b, tmp);
    buf_append_number(&b, r->metrics.reduction_percentage);
    buf_append(&b, "}");

    buf_append(&b, ",\"bytecode\":");
    serialize_bytecode(&b, r->bytecode);

    buf_append(&b, ",\"vmTrace\":");
    serialize_vm_trace(&b, r->vm);

    buf_append(&b, ",\"vmTraceTruncated\":");
    buf_append(&b, (r->vm && r->vm->truncated) ? "true" : "false");

    buf_append(&b, ",\"waitingForInput\":");
    buf_append(&b, (r->vm && r->vm->waitingForInput) ? "true" : "false");

    buf_append(&b, ",\"inputPrompt\":\"");
    if (r->vm) buf_append_escaped(&b, r->vm->inputPrompt);
    buf_append(&b, "\"");

    buf_append(&b, ",\"consoleOutput\":\"");
    if (r->vm) buf_append_escaped(&b, r->vm->consoleOutput);
    buf_append(&b, "\"");

    snprintf(tmp, sizeof(tmp), ",\"exitCode\":%d", r->vm ? r->vm->exitCode : 0);
    buf_append(&b, tmp);

    buf_append(&b, ",\"diagnostics\":");
    serialize_diagnostics(&b, r->diags);

    buf_append(&b, "}");
    return b.data;
}

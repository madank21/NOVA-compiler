#include "compile.h"
#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------- */
/* Growable output buffer                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    char* data;
    int size;
    int capacity;
} Buffer;

static void buf_init(Buffer* b) {
    b->capacity = 8192;
    b->size = 0;
    b->data = (char*)malloc((size_t)b->capacity);
    if (!b->data) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    b->data[0] = '\0';
}

static void buf_reserve(Buffer* b, int extra) {
    while (b->size + extra + 1 >= b->capacity) {
        b->capacity *= 2;
        char* grown = (char*)realloc(b->data, (size_t)b->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        b->data = grown;
    }
}

static void buf_append(Buffer* b, const char* s) {
    int len = (int)strlen(s);
    buf_reserve(b, len);
    memcpy(b->data + b->size, s, (size_t)len);
    b->size += len;
    b->data[b->size] = '\0';
}

static void buf_append_char(Buffer* b, char c) {
    buf_reserve(b, 1);
    b->data[b->size++] = c;
    b->data[b->size] = '\0';
}

static void buf_append_number(Buffer* b, double v) {
    char tmp[64];
    format_value(v, tmp, sizeof(tmp));
    buf_append(b, tmp);
}

/* JSON string escaping compatible with JSON.parse. */
static void buf_append_escaped(Buffer* b, const char* s) {
    if (!s) return;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"': buf_append(b, "\\\""); break;
            case '\\': buf_append(b, "\\\\"); break;
            case '\n': buf_append(b, "\\n"); break;
            case '\r': buf_append(b, "\\r"); break;
            case '\t': buf_append(b, "\\t"); break;
            case '\b': buf_append(b, "\\b"); break;
            case '\f': buf_append(b, "\\f"); break;
            default:
                if (*p < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    buf_append(b, tmp);
                } else {
                    buf_append_char(b, (char)*p);
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Section serializers                                                        */
/* ------------------------------------------------------------------------- */

static void serialize_tokens(Buffer* b, const TokenList* tokens) {
    buf_append(b, "[");
    for (int i = 0; i < tokens->count; i++) {
        if (i) buf_append(b, ",");
        const Token* t = &tokens->items[i];
        buf_append(b, "{\"type\":\"");
        buf_append(b, token_type_name(t->type));
        buf_append(b, "\",\"lexeme\":\"");
        buf_append_escaped(b, t->lexeme);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d,\"column\":%d}", t->line, t->column);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_ast_node(Buffer* b, const ASTNode* node) {
    if (!node) {
        buf_append(b, "null");
        return;
    }
    buf_append(b, "{\"type\":\"");
    buf_append(b, node->node_type);
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
    char tmp[32];
    snprintf(tmp, sizeof(tmp), ",\"line\":%d", node->line);
    buf_append(b, tmp);
    buf_append(b, ",\"children\":[");
    for (int i = 0; i < node->child_count; i++) {
        if (i) buf_append(b, ",");
        serialize_ast_node(b, node->children[i]);
    }
    buf_append(b, "]}");
}

static void serialize_symbol_table(Buffer* b, const SemModel* sem) {
    buf_append(b, "[");
    for (int i = 0; i < sem->symbol_count; i++) {
        if (i) buf_append(b, ",");
        const SymbolRow* row = &sem->symbols[i];
        buf_append(b, "{\"scope\":\"");
        buf_append_escaped(b, row->scope);
        buf_append(b, "\",\"name\":\"");
        buf_append_escaped(b, row->name);
        buf_append(b, "\",\"kind\":\"");
        buf_append(b, row->kind);
        buf_append(b, "\",\"type\":\"");
        buf_append_escaped(b, row->type);
        buf_append(b, "\",\"address\":\"");
        buf_append(b, row->address);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "\",\"params\":%d}", row->params);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_tac_list(Buffer* b, const TACList* list) {
    buf_append(b, "[");
    for (int i = 0; list && i < list->count; i++) {
        if (i) buf_append(b, ",");
        const TACInstr* ins = &list->items[i];
        char tmp[32];
        buf_append(b, "{\"op\":\"");
        buf_append(b, ins->op);
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

static void serialize_bytecode(Buffer* b, const BytecodeChunk* chunk) {
    buf_append(b, "[");
    for (int i = 0; chunk && i < chunk->count; i++) {
        if (i) buf_append(b, ",");
        const BInstr* ins = &chunk->code[i];
        buf_append(b, "{\"pc\":");
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", ins->pc);
        buf_append(b, tmp);
        buf_append(b, ",\"op\":\"");
        buf_append(b, ins->op);
        buf_append(b, "\",\"operand\":");
        buf_append_number(b, ins->operand);
        buf_append(b, ",\"symbol\":\"");
        buf_append_escaped(b, ins->symbol);
        snprintf(tmp, sizeof(tmp), "\",\"line\":%d}", ins->line);
        buf_append(b, tmp);
    }
    buf_append(b, "]");
}

static void serialize_vm_trace(Buffer* b, const VMResult* vm) {
    buf_append(b, "[");
    for (int i = 0; vm && i < vm->count; i++) {
        if (i) buf_append(b, ",");
        const VMStep* step = &vm->steps[i];
        char tmp[64];
        buf_append(b, "{\"step\":");
        snprintf(tmp, sizeof(tmp), "%d", step->step);
        buf_append(b, tmp);
        buf_append(b, ",\"pc\":");
        snprintf(tmp, sizeof(tmp), "%d", step->pc);
        buf_append(b, tmp);
        buf_append(b, ",\"line\":");
        snprintf(tmp, sizeof(tmp), "%d", step->line);
        buf_append(b, tmp);
        buf_append(b, ",\"instruction\":\"");
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
            buf_append(b, step->frames[j].ret_addr);
            buf_append(b, "\"}");
        }
        buf_append(b, "],\"console\":\"");
        buf_append_escaped(b, step->console);
        buf_append(b, "\"}");
    }
    buf_append(b, "]");
}

static void serialize_diagnostics(Buffer* b, const DiagList* diags) {
    buf_append(b, "[");
    for (int i = 0; diags && i < diags->count; i++) {
        if (i) buf_append(b, ",");
        const Diag* d = &diags->items[i];
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

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

char* serialize_result_json(const CompileResult* r) {
    Buffer b;
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
    if (r->sem) serialize_symbol_table(&b, r->sem);
    else buf_append(&b, "[{\"scope\":\"global\",\"name\":\"printf\",\"kind\":\"Function\",\"type\":\"int\",\"address\":\"0x0000\",\"params\":-1},"
                          "{\"scope\":\"global\",\"name\":\"scanf\",\"kind\":\"Function\",\"type\":\"int\",\"address\":\"0x0000\",\"params\":-1}]");

    buf_append(&b, ",\"tac\":");
    serialize_tac_list(&b, r->tac_gen ? r->tac_gen->instrs : NULL);

    buf_append(&b, ",\"optTac\":");
    serialize_tac_list(&b, r->opt_tac);

    snprintf(tmp, sizeof(tmp),
             ",\"metrics\":{\"constant_fold\":%d,\"constant_prop\":%d,\"dead_code\":%d,"
             "\"strength_reduce\":%d,\"reduction_percentage\":",
             r->metrics.constant_fold, r->metrics.constant_prop, r->metrics.dead_code,
             r->metrics.strength_reduce);
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
    buf_append(&b, (r->vm && r->vm->waiting_for_input) ? "true" : "false");

    buf_append(&b, ",\"inputPrompt\":\"");
    if (r->vm) buf_append_escaped(&b, r->vm->input_prompt);
    buf_append(&b, "\"");

    buf_append(&b, ",\"consoleOutput\":\"");
    if (r->vm) buf_append_escaped(&b, r->vm->console_output);
    buf_append(&b, "\"");

    snprintf(tmp, sizeof(tmp), ",\"exitCode\":%d", r->vm ? r->vm->exit_code : 0);
    buf_append(&b, tmp);

    buf_append(&b, ",\"diagnostics\":");
    serialize_diagnostics(&b, r->diags);

    buf_append(&b, "}");

    return b.data;
}
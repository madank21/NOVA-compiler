#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char* data;
    int size;
    int capacity;
} Buffer;

static Buffer* buf_init() {
    Buffer* b = (Buffer*)malloc(sizeof(Buffer));
    b->capacity = 4096;
    b->size = 0;
    b->data = (char*)malloc(b->capacity);
    b->data[0] = '\0';
    return b;
}

static void buf_append(Buffer* b, const char* str) {
    int len = (int)strlen(str);
    while (b->size + len + 1 >= b->capacity) {
        b->capacity *= 2;
        b->data = (char*)realloc(b->data, b->capacity);
    }
    strcpy(b->data + b->size, str);
    b->size += len;
}

static void buf_append_escaped(Buffer* b, const char* str) {
    if (!str) return;
    while (*str) {
        switch (*str) {
            case '"': buf_append(b, "\\\""); break;
            case '\\': buf_append(b, "\\\\"); break;
            case '\n': buf_append(b, "\\n"); break;
            case '\r': buf_append(b, "\\r"); break;
            case '\t': buf_append(b, "\\t"); break;
            default:
                if ((unsigned char)*str < 0x20) {
                    char temp[8];
                    snprintf(temp, sizeof(temp), "\\u%04x", (unsigned char)*str);
                    buf_append(b, temp);
                } else {
                    char tmp[2] = {*str, '\0'};
                    buf_append(b, tmp);
                }
                break;
        }
        str++;
    }
}

static const char* symbol_kind(Symbol* sym) {
    return sym->is_function ? "Function" : "Variable";
}

static void serialize_ast_node(Buffer* b, ASTNode* node) {
    if (!node) {
        buf_append(b, "null");
        return;
    }
    char temp[512];
    snprintf(temp, sizeof(temp), "{\"type\":\"%s\",\"identifier\":\"%s\",\"type_name\":\"%s\",\"op\":\"%s\",\"line\":%d,\"int_val\":%lld,\"float_val\":%.2f,\"children\":[",
             ast_node_type_to_string(node->type), node->identifier, node->type_name, node->op, node->line, node->int_val, node->float_val);
    buf_append(b, temp);

    int written = 0;
    if (node->left) {
        serialize_ast_node(b, node->left);
        written++;
    }
    if (node->right) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->right);
    }
    if (node->condition) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->condition);
    }
    if (node->else_branch) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->else_branch);
    }
    if (node->init) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->init);
    }
    if (node->increment) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->increment);
    }
    for (int i = 0; i < node->child_count; i++) {
        if (written++) buf_append(b, ",");
        serialize_ast_node(b, node->children[i]);
    }
    buf_append(b, "]}");
}

static void serialize_symbol_table(Buffer* b, SymbolTable* st) {
    buf_append(b, "[");
    int first = 1;
    for (Scope* scope = st ? st->scope_list : NULL; scope; scope = scope->next) {
        for (Symbol* sym = scope->symbols; sym; sym = sym->next) {
            if (!first) buf_append(b, ",");
            first = 0;
            buf_append(b, "{\"scope\":\"");
            buf_append_escaped(b, scope->name);
            buf_append(b, "\",\"name\":\"");
            buf_append_escaped(b, sym->name);
            buf_append(b, "\",\"kind\":\"");
            buf_append_escaped(b, symbol_kind(sym));
            buf_append(b, "\",\"type\":\"");
            buf_append_escaped(b, sym->type);
            buf_append(b, "\",\"address\":\"");
            if (sym->address != 0) {
                snprintf(temp, sizeof(temp), "0x%04X", sym->address);
            } else {
                snprintf(temp, sizeof(temp), "0x0000");
            }
            buf_append(b, temp);
            buf_append(b, "\",\"params\":");
            snprintf(temp, sizeof(temp), "%d", sym->param_count);
            buf_append(b, temp);
            buf_append(b, "}");
        }
    }
    buf_append(b, "]");
}

static void serialize_tac_list(Buffer* b, TACList* list) {
    buf_append(b, "[");
    int first = 1;
    for (TACInstr* curr = list ? list->head : NULL; curr; curr = curr->next) {
        if (!first) buf_append(b, ",");
        first = 0;
        char temp[512];
        snprintf(temp, sizeof(temp), "{\"op\":\"%s\",\"res\":\"%s\",\"a1\":\"%s\",\"a2\":\"%s\",\"line\":%d}",
                 tac_op_to_string(curr->op), curr->result, curr->arg1, curr->arg2, curr->line);
        buf_append(b, temp);
    }
    buf_append(b, "]");
}

static void serialize_vm_trace(Buffer* b, VMExecutionTrace* trace) {
    buf_append(b, "[");
    for (int i = 0; trace && i < trace->count; i++) {
        if (i) buf_append(b, ",");
        VMStep* step = &trace->steps[i];
        buf_append(b, "{\"step\":");
        char temp[512];
        snprintf(temp, sizeof(temp), "%d,\"pc\":%d,\"line\":%d,\"instruction\":\"EXEC_LINE %d\",\"stack\":[",
                 i, step->pc, step->line, step->line);
        buf_append(b, temp);
        for (int j = 0; j < step->stack_top; j++) {
            if (j) buf_append(b, ",");
            snprintf(temp, sizeof(temp), "%d", step->operand_stack[j]);
            buf_append(b, temp);
        }
        buf_append(b, "],\"variables\":[");
        for (int j = 0; j < step->var_count; j++) {
            if (j) buf_append(b, ",");
            snprintf(temp, sizeof(temp), "{\"name\":\"%s\",\"value\":%d}", step->variables[j].name, step->variables[j].value);
            buf_append(b, temp);
        }
        buf_append(b, "],\"console\":\"");
        buf_append_escaped(b, step->console_output);
        buf_append(b, "\"}");
    }
    buf_append(b, "]");
}

char* serialize_compilation_result(
    TokenList* tokens,
    ASTNode* ast,
    SymbolTable* st,
    TACList* tac,
    TACList* opt_tac,
    OptimizationMetrics* metrics,
    BytecodeChunk* bytecode,
    VMExecutionTrace* trace,
    double compile_time_ms
) {
    Buffer* b = buf_init();
    char temp[512];

    buf_append(b, "{\"success\":true,\"compile_time_ms\":");
    snprintf(temp, sizeof(temp), "%.2f", compile_time_ms);
    buf_append(b, temp);

    buf_append(b, ",\"tokens\":[");
    for (int i = 0; tokens && i < tokens->count; i++) {
        if (i) buf_append(b, ",");
        Token tok = tokens->tokens[i];
        buf_append(b, "{\"type\":\"");
        buf_append_escaped(b, token_type_to_string(tok.type));
        buf_append(b, "\",\"lexeme\":\"");
        buf_append_escaped(b, tok.lexeme);
        snprintf(temp, sizeof(temp), "\",\"line\":%d,\"column\":%d}", tok.line, tok.column);
        buf_append(b, temp);
    }
    buf_append(b, "]");

    buf_append(b, ",\"ast\":");
    serialize_ast_node(b, ast);

    buf_append(b, ",\"symbolTable\":");
    serialize_symbol_table(b, st);

    buf_append(b, ",\"tac\":");
    serialize_tac_list(b, tac);
    buf_append(b, ",\"optTac\":");
    serialize_tac_list(b, opt_tac);

    snprintf(temp, sizeof(temp), ",\"metrics\":{\"constant_fold\":%d,\"constant_prop\":%d,\"dead_code\":%d,\"strength_reduce\":%d,\"reduction_percentage\":%.1f}",
             metrics ? metrics->constant_fold_count : 0,
             metrics ? metrics->constant_prop_count : 0,
             metrics ? metrics->dead_code_count : 0,
             metrics ? metrics->strength_reduce_count : 0,
             metrics ? metrics->reduction_percentage : 0.0f);
    buf_append(b, temp);

    buf_append(b, ",\"bytecode\":[");
    if (bytecode) {
        for (int i = 0; i < bytecode->count; i++) {
            if (i) buf_append(b, ",");
            Instruction instr = bytecode->code[i];
            snprintf(temp, sizeof(temp), "{\"pc\":%d,\"op\":\"%s\",\"operand\":%d,\"symbol\":\"%s\",\"line\":%d}",
                     i, opcode_to_string(instr.op), instr.operand, instr.symbol, instr.line);
            buf_append(b, temp);
        }
    }
    buf_append(b, "]");

    buf_append(b, ",\"vmTrace\":");
    serialize_vm_trace(b, trace);

    buf_append(b, ",\"consoleOutput\":\"");
    if (trace && trace->count > 0) {
        buf_append_escaped(b, trace->steps[trace->count - 1].console_output);
    }
    buf_append(b, "\"");

    buf_append(b, "}");
    char* result = b->data;
    free(b);
    return result;
}

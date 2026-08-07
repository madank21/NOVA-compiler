#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void serialize_ast_node(Buffer* b, ASTNode* node) {
    if (!node) {
        buf_append(b, "null");
        return;
    }
    char temp[512];
    snprintf(temp, sizeof(temp), "{\"type\":\"%s\",\"identifier\":\"%s\",\"type_name\":\"%s\",\"op\":\"%s\",\"line\":%d,\"int_val\":%lld,\"float_val\":%.2f,\"children\":[",
             ast_node_type_to_string(node->type), node->identifier, node->type_name, node->op, node->line, node->int_val, node->float_val);
    buf_append(b, temp);

    if (node->left) {
        serialize_ast_node(b, node->left);
        if (node->right || node->condition || node->child_count > 0) buf_append(b, ",");
    }
    if (node->right) {
        serialize_ast_node(b, node->right);
        if (node->condition || node->child_count > 0) buf_append(b, ",");
    }
    if (node->condition) {
        serialize_ast_node(b, node->condition);
        if (node->child_count > 0) buf_append(b, ",");
    }
    for (int i = 0; i < node->child_count; i++) {
        serialize_ast_node(b, node->children[i]);
        if (i < node->child_count - 1) buf_append(b, ",");
    }
    buf_append(b, "]}");
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

    // Tokens
    buf_append(b, ",\"tokens\":[");
    for (int i = 0; i < tokens->count; i++) {
        Token tok = tokens->tokens[i];
        snprintf(temp, sizeof(temp), "{\"type\":\"%s\",\"lexeme\":\"%s\",\"line\":%d,\"column\":%d}",
                 token_type_to_string(tok.type), tok.lexeme, tok.line, tok.column);
        buf_append(b, temp);
        if (i < tokens->count - 1) buf_append(b, ",");
    }
    buf_append(b, "]");

    // AST
    buf_append(b, ",\"ast\":");
    serialize_ast_node(b, ast);

    // Metrics
    snprintf(temp, sizeof(temp), ",\"metrics\":{\"constant_fold\":%d,\"constant_prop\":%d,\"dead_code\":%d,\"strength_reduce\":%d,\"reduction_percentage\":%.1f}",
             metrics->constant_fold_count, metrics->constant_prop_count, metrics->dead_code_count, metrics->strength_reduce_count, metrics->reduction_percentage);
    buf_append(b, temp);

    // Bytecode
    buf_append(b, ",\"bytecode\":[");
    for (int i = 0; i < bytecode->count; i++) {
        Instruction instr = bytecode->code[i];
        snprintf(temp, sizeof(temp), "{\"pc\":%d,\"op\":\"%s\",\"operand\":%d,\"symbol\":\"%s\",\"line\":%d}",
                 i, opcode_to_string(instr.op), instr.operand, instr.symbol, instr.line);
        buf_append(b, temp);
        if (i < bytecode->count - 1) buf_append(b, ",");
    }
    buf_append(b, "]");

    // VM Trace
    buf_append(b, ",\"vm_trace\":[");
    for (int i = 0; i < trace->count; i++) {
        VMStep step = trace->steps[i];
        snprintf(temp, sizeof(temp), "{\"pc\":%d,\"line\":%d,\"stack_top\":%d,\"variables\":[", step.pc, step.line, step.stack_top);
        buf_append(b, temp);

        for (int j = 0; j < step.var_count; j++) {
            snprintf(temp, sizeof(temp), "{\"name\":\"%s\",\"value\":%d}", step.variables[j].name, step.variables[j].value);
            buf_append(b, temp);
            if (j < step.var_count - 1) buf_append(b, ",");
        }
        buf_append(b, "]}");
        if (i < trace->count - 1) buf_append(b, ",");
    }
    buf_append(b, "]");

    buf_append(b, "}");
    char* result = b->data;
    free(b);
    return result;
}

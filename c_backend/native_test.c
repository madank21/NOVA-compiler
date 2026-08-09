#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "tac.h"
#include "optimizer.h"
#include "bytecode.h"
#include "vm.h"

static void print_test_result(const char* name, int ok, const char* message) {
    printf("[%s] %s - %s\n", ok ? "PASS" : "FAIL", name, message);
}

int main(void) {
    const char* source =
        "#include <stdio.h>\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    int y = 10;\n"
        "    int z = x + y * 2;\n"
        "    printf(\"%d\\n\", z);\n"
        "    return 0;\n"
        "}\n";

    Lexer* lexer = lexer_init(source);
    if (!lexer) {
        print_test_result("lexer_init", 0, "failed to allocate lexer");
        return 1;
    }

    TokenList* tokens = lexer_tokenize(lexer);
    if (!tokens || tokens->count == 0) {
        print_test_result("tokenize", 0, "no tokens generated");
        lexer_free(lexer);
        return 1;
    }
    print_test_result("tokenize", 1, "tokens generated");

    Parser* parser = parser_init(tokens);
    if (!parser) {
        print_test_result("parser_init", 0, "failed to allocate parser");
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }

    ASTNode* ast = parse_program(parser);
    if (!ast) {
        print_test_result("parse_program", 0, "AST generation failed");
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("parse_program", 1, "AST generated");

    SymbolTable* st = semantic_analyze(ast);
    if (!st) {
        print_test_result("semantic_analyze", 0, "symbol table creation failed");
        ast_free(ast);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("semantic_analyze", 1, "symbol table produced");

    TACList* tac = generate_tac(ast);
    if (!tac || tac->count == 0) {
        print_test_result("generate_tac", 0, "TAC generation failed");
        symbol_table_free(st);
        ast_free(ast);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("generate_tac", 1, "TAC generated");

    OptimizationMetrics metrics;
    TACList* opt_tac = optimize_tac(tac, &metrics);
    if (!opt_tac || opt_tac->count == 0) {
        print_test_result("optimize_tac", 0, "optimization failed");
        tac_list_free(tac);
        symbol_table_free(st);
        ast_free(ast);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("optimize_tac", 1, "TAC optimized");

    BytecodeChunk* bytecode = generate_bytecode(opt_tac);
    if (!bytecode || bytecode->count == 0) {
        print_test_result("generate_bytecode", 0, "bytecode generation failed");
        tac_list_free(opt_tac);
        tac_list_free(tac);
        symbol_table_free(st);
        ast_free(ast);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("generate_bytecode", 1, "bytecode generated");

    VMExecutionTrace* trace = vm_execute(bytecode);
    if (!trace || trace->count == 0) {
        print_test_result("vm_execute", 0, "VM execution failed");
        bytecode_chunk_free(bytecode);
        tac_list_free(opt_tac);
        tac_list_free(tac);
        symbol_table_free(st);
        ast_free(ast);
        parser_free(parser);
        token_list_free(tokens);
        lexer_free(lexer);
        return 1;
    }
    print_test_result("vm_execute", 1, "VM executed");

    int found_printf = 0;
    for (int i = 0; trace && i < trace->count; i++) {
        if (strstr(trace->steps[i].console_output, "15") != NULL) {
            found_printf = 1;
            break;
        }
    }
    print_test_result("printf_output", found_printf, found_printf ? "console output contains result" : "expected output missing");

    vm_trace_free(trace);
    bytecode_chunk_free(bytecode);
    tac_list_free(opt_tac);
    tac_list_free(tac);
    symbol_table_free(st);
    ast_free(ast);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return found_printf ? 0 : 1;
}

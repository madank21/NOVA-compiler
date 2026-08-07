#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "tac.h"
#include "optimizer.h"
#include "bytecode.h"
#include "vm.h"
#include "json.h"

int main(int argc, char** argv) {
    printf("Nova Studio Pure C Compiler Backend Engine v1.0.0\n");
    const char* sample_c_code =
        "#include <stdio.h>\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    int y = 10;\n"
        "    int z = x + y * 2;\n"
        "    printf(\"%d\\n\", z);\n"
        "    return 0;\n"
        "}\n";

    clock_t start = clock();

    Lexer* lexer = lexer_init(sample_c_code);
    TokenList* tokens = lexer_tokenize(lexer);

    Parser* parser = parser_init(tokens);
    ASTNode* ast = parse_program(parser);

    SymbolTable* st = semantic_analyze(ast);

    TACList* tac = generate_tac(ast);
    OptimizationMetrics metrics;
    TACList* opt_tac = optimize_tac(tac, &metrics);

    BytecodeChunk* bytecode = generate_bytecode(opt_tac);
    VMExecutionTrace* trace = vm_execute(bytecode);

    clock_t end = clock();
    double compile_time_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;

    char* json_output = serialize_compilation_result(tokens, ast, st, tac, opt_tac, &metrics, bytecode, trace, compile_time_ms);
    printf("Compilation completed in %.2f ms!\n", compile_time_ms);
    printf("Generated JSON Output (%zu bytes):\n%.200s...\n", strlen(json_output), json_output);

    free(json_output);
    vm_trace_free(trace);
    bytecode_chunk_free(bytecode);
    tac_list_free(opt_tac);
    tac_list_free(tac);
    symbol_table_free(st);
    ast_free(ast);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);

    return 0;
}

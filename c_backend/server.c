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
#include "json.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#define PORT 8080

// Extract C source from raw HTTP body or JSON {"source": "..."}
static const char* parse_source_code(const char* body, char* out_buf, size_t max_len) {
    const char* json_key = strstr(body, "\"source\"");
    if (json_key) {
        const char* val_start = strchr(json_key, ':');
        if (val_start) {
            val_start = strchr(val_start, '"');
            if (val_start) {
                val_start++;
                size_t idx = 0;
                while (*val_start && *val_start != '"' && idx < max_len - 1) {
                    if (*val_start == '\\' && *(val_start + 1) != '\0') {
                        val_start++;
                        if (*val_start == 'n') out_buf[idx++] = '\n';
                        else if (*val_start == 't') out_buf[idx++] = '\t';
                        else out_buf[idx++] = *val_start;
                    } else {
                        out_buf[idx++] = *val_start;
                    }
                    val_start++;
                }
                out_buf[idx] = '\0';
                return out_buf;
            }
        }
    }
    return body;
}

void handle_request(socket_t client_socket, const char* raw_body) {
    clock_t start = clock();

    char extracted_code[4096] = {0};
    const char* code_to_compile = parse_source_code(raw_body, extracted_code, sizeof(extracted_code));

    Lexer* lexer = lexer_init(code_to_compile);
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

    char* json_res = serialize_compilation_result(tokens, ast, st, tac, opt_tac, &metrics, bytecode, trace, compile_time_ms);

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Length: %zu\r\n"
        "\r\n", strlen(json_res));

    send(client_socket, header, (int)strlen(header), 0);
    send(client_socket, json_res, (int)strlen(json_res), 0);

    free(json_res);
    vm_trace_free(trace);
    bytecode_chunk_free(bytecode);
    tac_list_free(opt_tac);
    tac_list_free(tac);
    symbol_table_free(st);
    ast_free(ast);
    parser_free(parser);
    token_list_free(tokens);
    lexer_free(lexer);
}

int start_server() {
#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        printf("Failed to create socket\n");
        return 1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        printf("Bind failed on port %d\n", PORT);
        return 1;
    }

    if (listen(server_fd, 10) == SOCKET_ERROR) {
        printf("Listen failed\n");
        return 1;
    }

    printf("Nova Studio Pure C HTTP Server listening on http://localhost:%d\n", PORT);

    while (1) {
        socket_t client_socket = accept(server_fd, NULL, NULL);
        if (client_socket == INVALID_SOCKET) continue;

        char buffer[4096] = {0};
        recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (strstr(buffer, "OPTIONS")) {
            const char* cors_response =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n\r\n";
            send(client_socket, cors_response, (int)strlen(cors_response), 0);
        } else {
            char* body = strstr(buffer, "\r\n\r\n");
            if (body) body += 4;
            else body = "int main() { return 0; }";
            handle_request(client_socket, body);
        }
        closesocket(client_socket);
    }

#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}

/* nova_server — hardened HTTP API for the native NOVA compiler backend.
 *
 * Endpoints:
 *   POST /api/compile   body: {"source": "...", "inputs": ["42", ...]}
 *                       (a raw text/plain body is also accepted as source)
 *   GET  /api/health    -> 200 {"status":"ok"}
 *   GET  /api/version   -> 200 {"engine":"native-c","version":"2.0.0"}
 *
 * Hardening (audit items B-4/B-5):
 *   - binds 127.0.0.1 by default (NOVA_HOST to override)
 *   - Content-Length aware request reading with a 256 KB body cap (413 beyond)
 *   - recv/send timeouts (slowloris protection)
 *   - SO_REUSEADDR
 *   - per-connection thread (POSIX), single-threaded fallback on Windows
 *   - real routing with 404/405 responses
 *   - request logging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compile.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSESOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define CLOSESOCKET close
#endif

#define MAX_BODY_BYTES (256 * 1024)
#define RECV_TIMEOUT_SEC 10
#define SEND_TIMEOUT_SEC 10
#define NOVA_SERVER_VERSION "2.0.0"

/* ------------------------------------------------------------------------- */
/* Minimal JSON extraction for {"source": "...", "inputs": [...]}             */
/* ------------------------------------------------------------------------- */

typedef struct {
    char* source;               /* malloc'd, or NULL to use raw body */
    char** inputs;              /* malloc'd array of malloc'd strings */
    int input_count;
} RequestPayload;

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string starting at p (which must point at '"').
 * Returns end pointer (after closing quote) or NULL on malformed input.
 * If out != NULL, stores a malloc'd decoded copy. */
static const char* json_parse_string(const char* p, char** out) {
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    while (*p && *p != '"') {
        if (len + 8 >= cap) {
            cap *= 2;
            char* grown = (char*)realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': buf[len++] = '"'; p++; break;
                case '\\': buf[len++] = '\\'; p++; break;
                case '/': buf[len++] = '/'; p++; break;
                case 'b': buf[len++] = '\b'; p++; break;
                case 'f': buf[len++] = '\f'; p++; break;
                case 'n': buf[len++] = '\n'; p++; break;
                case 'r': buf[len++] = '\r'; p++; break;
                case 't': buf[len++] = '\t'; p++; break;
                case 'u': {
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        p++;
                        char c = *p;
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                        else { free(buf); return NULL; }
                    }
                    p++;
                    /* encode UTF-8 (BMP only; surrogate pairs handled as replacement char) */
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        buf[len++] = '?';
                    } else if (cp < 0x80) {
                        buf[len++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: free(buf); return NULL;
            }
        } else {
            buf[len++] = *p++;
        }
    }
    if (*p != '"') { free(buf); return NULL; }
    buf[len] = '\0';
    if (out) *out = buf;
    else free(buf);
    return p + 1;
}

/* Skip any JSON value. Returns end pointer or NULL. */
static const char* json_skip_value(const char* p) {
    p = json_skip_ws(p);
    if (*p == '"') return json_parse_string(p, NULL);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = json_parse_string(p, NULL);
                if (!p) return NULL;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    /* literal / number */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

static int payload_add_input(RequestPayload* pl, char* s) {
    char** grown = (char**)realloc(pl->inputs, sizeof(char*) * (size_t)(pl->input_count + 1));
    if (!grown) { free(s); return 0; }
    pl->inputs = grown;
    pl->inputs[pl->input_count++] = s;
    return 1;
}

/* Parse the request body. Returns 1 when a JSON object was parsed,
 * 0 when the body should be treated as raw source text. */
static int parse_request_payload(const char* body, size_t body_len, RequestPayload* pl) {
    (void)body_len; /* body is NUL-terminated by the reader */
    memset(pl, 0, sizeof(RequestPayload));
    const char* p = json_skip_ws(body);
    if (*p != '{') return 0;
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == '}') break;
        if (*p != '"') return pl->source ? 1 : 0; /* tolerate trailing junk */
        char* key = NULL;
        p = json_parse_string(p, &key);
        if (!p) { free(key); break; }
        p = json_skip_ws(p);
        if (*p != ':') { free(key); break; }
        p++;
        p = json_skip_ws(p);
        if (key && strcmp(key, "source") == 0) {
            char* val = NULL;
            const char* end = json_parse_string(p, &val);
            if (end) { free(pl->source); pl->source = val; p = end; }
            else { free(key); break; }
        } else if (key && strcmp(key, "inputs") == 0) {
            if (*p == '[') {
                p++;
                for (;;) {
                    p = json_skip_ws(p);
                    if (*p == ']') { p++; break; }
                    if (*p == '"') {
                        char* val = NULL;
                        const char* end = json_parse_string(p, &val);
                        if (!end) { free(key); return pl->source ? 1 : 0; }
                        payload_add_input(pl, val);
                        p = end;
                    } else {
                        /* number / literal -> stringify up to delimiter */
                        const char* start = p;
                        while (*p && *p != ',' && *p != ']') p++;
                        size_t n = (size_t)(p - start);
                        char* val = (char*)malloc(n + 1);
                        memcpy(val, start, n);
                        val[n] = '\0';
                        /* trim trailing spaces */
                        while (n > 0 && (val[n-1] == ' ' || val[n-1] == '\t')) val[--n] = '\0';
                        payload_add_input(pl, val);
                    }
                    p = json_skip_ws(p);
                    if (*p == ',') { p++; continue; }
                }
            } else {
                p = json_skip_value(p);
                if (!p) break;
            }
        } else {
            const char* end = json_skip_value(p);
            if (!end) { free(key); break; }
            p = end;
        }
        free(key);
        p = json_skip_ws(p);
        if (*p == ',') { p++; continue; }
    }
    return pl->source != NULL;
}

static void payload_free(RequestPayload* pl) {
    free(pl->source);
    for (int i = 0; i < pl->input_count; i++) free(pl->inputs[i]);
    free(pl->inputs);
}

/* ------------------------------------------------------------------------- */
/* HTTP plumbing                                                              */
/* ------------------------------------------------------------------------- */

static int send_all(socket_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(s, data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static void send_response(socket_t s, int status, const char* status_text,
                          const char* content_type, const char* body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, (unsigned long)body_len);
    send_all(s, header, (size_t)hlen);
    if (body && body_len) send_all(s, body, body_len);
}

/* Read exactly until \r\n\r\n, honoring Content-Length for the body.
 * Returns malloc'd buffer (NUL-terminated) and sets *header_end / *body_len.
 * Returns NULL on timeout/overflow. */
static char* read_request(socket_t s, size_t* out_total, size_t* out_body_offset, size_t* out_body_len) {
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    size_t header_end_off = 0;
    size_t content_length = 0;
    int have_header = 0;

    for (;;) {
        if (len + 1024 >= cap) {
            if (cap >= MAX_BODY_BYTES + 65536) { free(buf); return NULL; }
            cap *= 2;
            char* grown = (char*)realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        int n = (int)recv(s, buf + len, 1024, 0);
        if (n <= 0) {
            if (len > 0 && have_header && len >= header_end_off + content_length) break;
            free(buf);
            return NULL;
        }
        len += (size_t)n;
        buf[len] = '\0';

        if (!have_header) {
            char* hend = strstr(buf, "\r\n\r\n");
            if (hend) {
                header_end_off = (size_t)(hend - buf) + 4;
                have_header = 1;
                /* parse Content-Length (case-insensitive, header region only) */
                char saved = buf[header_end_off];
                buf[header_end_off] = '\0';
                const char* needle = "content-length:";
                char* header_lower = (char*)malloc(header_end_off + 1);
                if (header_lower) {
                    for (size_t i = 0; i < header_end_off; i++) {
                        char c = buf[i];
                        header_lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                    }
                    header_lower[header_end_off] = '\0';
                    char* cl = strstr(header_lower, needle);
                    if (cl) content_length = (size_t)strtoul(cl + strlen(needle), NULL, 10);
                    free(header_lower);
                }
                buf[header_end_off] = saved;
                if (content_length > MAX_BODY_BYTES) {
                    free(buf);
                    return NULL; /* caller sends 413 */
                }
            }
        }
        if (have_header && len >= header_end_off + content_length) {
            /* trim any pipelined extra bytes */
            len = header_end_off + content_length;
            buf[len] = '\0';
            break;
        }
        if (len > MAX_BODY_BYTES + 65536) { free(buf); return NULL; }
    }

    *out_total = len;
    *out_body_offset = header_end_off;
    *out_body_len = len - header_end_off;
    return buf;
}

static void log_request(const char* method, const char* path, int status, unsigned long bytes, double ms) {
    time_t now = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    tm_buf = *localtime(&now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    printf("[%s] %s %s -> %d (%lu bytes, %.1f ms)\n", ts, method, path, status, bytes, ms);
    fflush(stdout);
}

typedef struct {
    socket_t client;
} ConnArg;

static void handle_connection(socket_t client) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t total = 0, body_off = 0, body_len = 0;
    char* req = read_request(client, &total, &body_off, &body_len);
    if (!req) {
        const char* msg = "{\"error\":\"request too large or malformed\"}";
        send_response(client, 413, "Payload Too Large", "application/json", msg, strlen(msg));
        log_request("?", "?", 413, strlen(msg), 0);
        CLOSESOCKET(client);
        return;
    }

    /* parse request line */
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    const char* body = req + body_off;

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client, 204, "No Content", "text/plain", "", 0);
        log_request(method, path, 204, 0, 0);
    } else if (strcmp(path, "/api/health") == 0 && strcmp(method, "GET") == 0) {
        const char* msg = "{\"status\":\"ok\"}";
        send_response(client, 200, "OK", "application/json", msg, strlen(msg));
        log_request(method, path, 200, strlen(msg), 0);
    } else if (strcmp(path, "/api/version") == 0 && strcmp(method, "GET") == 0) {
        const char* msg = "{\"engine\":\"native-c\",\"version\":\"" NOVA_SERVER_VERSION "\"}";
        send_response(client, 200, "OK", "application/json", msg, strlen(msg));
        log_request(method, path, 200, strlen(msg), 0);
    } else if (strcmp(path, "/api/compile") == 0 && strcmp(method, "POST") == 0) {
        RequestPayload pl;
        int is_json = parse_request_payload(body, body_len, &pl);
        const char* source = is_json && pl.source ? pl.source : body;
        CompileResult* result = compile_source(
            source,
            (const char**)(pl.input_count ? pl.inputs : NULL),
            pl.input_count);
        char* json = serialize_result_json(result);
        send_response(client, 200, "OK", "application/json", json, strlen(json));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        log_request(method, path, 200, strlen(json), ms);
        free(json);
        compile_result_free(result);
        payload_free(&pl);
    } else if (strcmp(path, "/api/compile") == 0) {
        const char* msg = "{\"error\":\"method not allowed\"}";
        send_response(client, 405, "Method Not Allowed", "application/json", msg, strlen(msg));
        log_request(method, path, 405, strlen(msg), 0);
    } else {
        const char* msg = "{\"error\":\"not found\"}";
        send_response(client, 404, "Not Found", "application/json", msg, strlen(msg));
        log_request(method, path, 404, strlen(msg), 0);
    }

    free(req);
    CLOSESOCKET(client);
}

#if !defined(_WIN32)
static void* connection_thread(void* arg) {
    ConnArg* ca = (ConnArg*)arg;
    handle_connection(ca->client);
    free(ca);
    return NULL;
}
#endif

int main(void) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    const char* host_env = getenv("NOVA_HOST");
    const char* port_env = getenv("NOVA_PORT");
    const char* host = (host_env && *host_env) ? host_env : "127.0.0.1";
    int port = (port_env && *port_env) ? atoi(port_env) : 8080;
    if (port <= 0 || port > 65535) port = 8080;

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        fprintf(stderr, "nova_server: failed to create socket\n");
        return 1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "nova_server: invalid NOVA_HOST '%s'\n", host);
        CLOSESOCKET(server_fd);
        return 1;
    }

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "nova_server: bind failed on %s:%d\n", host, port);
        CLOSESOCKET(server_fd);
        return 1;
    }
    if (listen(server_fd, 64) != 0) {
        fprintf(stderr, "nova_server: listen failed\n");
        CLOSESOCKET(server_fd);
        return 1;
    }

    printf("nova_server %s listening on http://%s:%d (POST /api/compile, GET /api/health)\n",
           NOVA_SERVER_VERSION, host, port);
    fflush(stdout);

    for (;;) {
        socket_t client = accept(server_fd, NULL, NULL);
        if (client == INVALID_SOCK) continue;

        /* per-connection timeouts */
#if !defined(_WIN32)
        struct timeval tv;
        tv.tv_sec = RECV_TIMEOUT_SEC; tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        tv.tv_sec = SEND_TIMEOUT_SEC;
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

#if defined(_WIN32)
        handle_connection(client);
#else
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) { CLOSESOCKET(client); continue; }
        ca->client = client;
        pthread_t th;
        if (pthread_create(&th, NULL, connection_thread, ca) != 0) {
            CLOSESOCKET(client);
            free(ca);
            continue;
        }
        pthread_detach(th);
#endif
    }

#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}
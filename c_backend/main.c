/* nova_compiler — CLI for the native NOVA compiler backend.
 *
 *   nova_compiler file.c          compile a file, print the JSON contract
 *   nova_compiler -               compile from stdin
 *   nova_compiler                 compile a built-in sample
 *
 * scanf inputs come from NOVA_INPUTS (newline-separated).
 * Exit code 0 on success, 1 on compile/runtime failure.
 */

#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *f) {
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
        if (cap - len < 2) {
            cap *= 2;
            char *g = (char *)realloc(buf, cap);
            if (!g) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
            buf = g;
        }
    }
    buf[len] = '\0';
    return buf;
}

static const char *SAMPLE =
    "#include <stdio.h>\n"
    "int main() {\n"
    "    int x = 5;\n"
    "    int y = 10;\n"
    "    int z = x + y * 2;\n"
    "    printf(\"z = %d\\n\", z);\n"
    "    return 0;\n"
    "}\n";

int main(int argc, char **argv) {
    char *source = NULL;
    char *heap_source = NULL;

    if (argc >= 2 && strcmp(argv[1], "-") == 0) {
        heap_source = read_all(stdin);
        source = heap_source;
    } else if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "nova: cannot open '%s'\n", argv[1]); return 2; }
        heap_source = read_all(f);
        fclose(f);
        source = heap_source;
    } else {
        source = (char *)SAMPLE;
    }

    const char *inputs[64];
    int input_count = 0;
    char *env_copy = NULL;
    const char *env_inputs = getenv("NOVA_INPUTS");
    if (env_inputs && *env_inputs) {
        env_copy = strdup(env_inputs);
        char *save = NULL;
        for (char *tok = strtok_r(env_copy, "\n", &save);
             tok && input_count < 64;
             tok = strtok_r(NULL, "\n", &save)) {
            inputs[input_count++] = tok;
        }
    }

    CompileResult *result = nova_compile(source, inputs, input_count);
    char *json = nova_to_json(result);
    fwrite(json, 1, strlen(json), stdout);
    fputc('\n', stdout);

    int rc = result->success ? 0 : 1;
    free(json);
    nova_compile_free(result);
    free(heap_source);
    free(env_copy);
    return rc;
}

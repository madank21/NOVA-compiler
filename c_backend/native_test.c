/* NOVA native backend test harness.
 * Mirrors tests/run_engine_test.mjs. Zero-dep; exits non-zero on failure. */

#include "compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

static void check(const char *name, int cond, const char *detail) {
    if (cond) { g_passed++; printf("[PASS] %s\n", name); }
    else { g_failed++; printf("[FAIL] %s%s%s\n", name, detail ? " — " : "", detail ? detail : ""); }
}

static void exact_console(const char *name, CompileResult *r, const char *expected) {
    if (!r->success) {
        char d[256];
        snprintf(d, sizeof(d), "compile failed (%d diagnostics)", r->diags->count);
        check(name, 0, d);
        return;
    }
    const char *actual = r->vm ? r->vm->consoleOutput : "";
    if (strcmp(actual, expected) != 0) {
        char d[512];
        snprintf(d, sizeof(d), "expected \"%s\", got \"%s\"", expected, actual);
        check(name, 0, d);
    } else {
        check(name, 1, NULL);
    }
}

typedef struct { const char *name; const char *code; const char *expected; int expectSuccess; } Case;

int main(void) {
    /* ---- valid programs: exact console output ---- */
    Case valid[] = {
        { "hello_world", "int main() { printf(\"Hello, World!\\n\"); return 0; }", "Hello, World!\n", 1 },
        { "arithmetic_precedence", "int main() { int x = 2 + 3 * 4; printf(\"%d\\n\", x); return 0; }", "14\n", 1 },
        { "parentheses", "int main() { int x = (2 + 3) * 4; printf(\"%d\\n\", x); return 0; }", "20\n", 1 },
        { "int_division", "int main() { printf(\"%d %d\\n\", 7 / 2, -7 / 2); return 0; }", "3 -3\n", 1 },
        { "modulo", "int main() { printf(\"%d %d\\n\", 7 % 3, -7 % 3); return 0; }", "1 -1\n", 1 },
        { "ternary", "int main() { int x = 5; printf(\"%d %d\\n\", x > 3 ? 10 : 20, x > 9 ? 1 : 0); return 0; }", "10 0\n", 1 },
        { "bitwise", "int main() { int a = 0x55, b = 0xAA; printf(\"%d %d %d %d %d\\n\", a & b, a | b, a ^ b, a << 2, ~a); return 0; }", "0 255 255 340 -86\n", 1 },
        { "switch_break", "int main() { int v = 2; switch (v) { case 1: printf(\"one\\n\"); break; case 2: printf(\"two\\n\"); break; default: printf(\"other\\n\"); } return 0; }", "two\n", 1 },
        { "switch_fallthrough", "int main() { int v = 1; switch (v) { case 1: printf(\"a \"); case 2: printf(\"b \"); break; case 3: printf(\"c \"); } printf(\"\\n\"); return 0; }", "a b \n", 1 },
        { "goto_label", "int main() { int i = 0; loop: i++; if (i < 3) goto loop; printf(\"%d\\n\", i); return 0; }", "3\n", 1 },
        { "do_while", "int main() { int i = 0; do { i++; } while (i < 5); printf(\"%d\\n\", i); return 0; }", "5\n", 1 },
        { "sizeof_types", "int main() { printf(\"%d %d %d %d\\n\", (int)sizeof(int), (int)sizeof(double), (int)sizeof(char), (int)sizeof(int *)); return 0; }", "4 8 1 8\n", 1 },
        { "casts", "int main() { double d = 3.9; int i = (int)d; printf(\"%d %d\\n\", i, (int)7.8); return 0; }", "3 7\n", 1 },
        { "string_concat", "int main() { char *s = \"Hello \" \"World\"; printf(\"%s\\n\", s); return 0; }", "Hello World\n", 1 },
        { "static_local", "int counter() { static int c = 0; c++; return c; } int main() { printf(\"%d %d %d\\n\", counter(), counter(), counter()); return 0; }", "1 2 3\n", 1 },
        { "unsigned_long_decls", "int main() { unsigned int u = 100; long long ll = 200; printf(\"%d %d\\n\", u, ll); return 0; }", "100 200\n", 1 },
        { "math_builtins", "int main() { printf(\"%d %d %d\\n\", (int)sqrt(16.0), (int)pow(2.0, 3.0), (int)fabs(-7.5)); return 0; }", "4 8 7\n", 1 },
        { "printf_formats", "int main() { printf(\"%x %X %o %u\\n\", 255, 255, 8, -1); return 0; }", "ff FF 10 4294967295\n", 1 },
        { "null_predefined", "int main() { int *p = NULL; if (p == NULL) printf(\"null\\n\"); return 0; }", "null\n", 1 },
        { "ifdef_excludes", "int main() {\n#ifdef __GNUC__\nthis would not parse;\n#endif\nprintf(\"ok\\n\"); return 0; }", "ok\n", 1 },
        { "assert_passes", "int main() { int x = 5; assert(x == 5); printf(\"ok\\n\"); return 0; }", "ok\n", 1 },
        { "compound_bitwise", "int main() { int x = 5; x &= 3; x |= 8; x ^= 1; x <<= 1; x >>= 1; printf(\"%d\\n\", x); return 0; }", "8\n", 1 },
        { "forward_decl", "int add(int a, int b);\nint main() { printf(\"%d\\n\", add(2, 3)); return 0; }\nint add(int a, int b) { return a + b; }", "5\n", 1 },
        { "while_factorial", "int main() { int n = 5, f = 1; while (n > 1) { f = f * n; n = n - 1; } printf(\"%d\\n\", f); return 0; }", "120\n", 1 },
        { "for_sum", "int main() { int s = 0; for (int i = 1; i <= 10; i = i + 1) s = s + i; printf(\"%d\\n\", s); return 0; }", "55\n", 1 },
        { "recursion_factorial", "int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { printf(\"%d\\n\", fact(6)); return 0; }", "720\n", 1 },
        { "array_sum", "int main() { int a[4] = {10, 20, 30, 40}; int s = 0; for (int i = 0; i < 4; i++) s += a[i]; printf(\"%d\\n\", s); return 0; }", "100\n", 1 },
        { "pointers", "int main() { int x = 5; int *p = &x; *p = *p + 2; printf(\"%d\\n\", x); return 0; }", "7\n", 1 },
        { "pointer_params", "void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; } int main() { int x = 1, y = 2; swap(&x, &y); printf(\"%d %d\\n\", x, y); return 0; }", "2 1\n", 1 },
        { "struct_member", "struct P { int x; }; int main() { struct P p; p.x = 7; printf(\"%d\\n\", p.x); return 0; }", "7\n", 1 },
        { "break_continue", "int main() { int s = 0; for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; s += i; } printf(\"%d\\n\", s); return 0; }", "18\n", 1 },
        { "char_literal", "int main() { char c = 'A'; printf(\"%c %d\\n\", c, c + 1); return 0; }", "A 66\n", 1 },
    };
    int nValid = (int)(sizeof(valid) / sizeof(valid[0]));
    for (int i = 0; i < nValid; i++) {
        CompileResult *r = nova_compile(valid[i].code, NULL, 0);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s: success", valid[i].name);
        check(nm, r->success == 1, r->success ? NULL : "compile failed");
        if (r->success) {
            snprintf(nm, sizeof(nm), "%s: console", valid[i].name);
            const char *actual = r->vm ? r->vm->consoleOutput : "";
            if (strcmp(actual, valid[i].expected) != 0) {
                char d[512];
                snprintf(d, sizeof(d), "expected \"%s\", got \"%s\"", valid[i].expected, actual);
                check(nm, 0, d);
            } else check(nm, 1, NULL);
        }
        nova_compile_free(r);
    }

    /* ---- invalid programs: must fail with diagnostics, never crash ---- */
    const char *invalid[][2] = {
        { "empty_source", "" },
        { "missing_main", "int foo() { return 1; }" },
        { "undefined_identifier", "int main() { int x = y + 1; return 0; }" },
        { "missing_semicolon", "int main() { int x = 5 return 0; }" },
        { "unbalanced_brace", "int main() { { return 0; }" },
        { "assignment_to_literal", "int main() { 5 = 3; return 0; }" },
        { "wrong_arg_count", "int f(int a) { return a; }\nint main() { return f(1, 2); }" },
        { "break_outside_loop", "int main() { break; return 0; }" },
        { "typedef_rejected", "typedef int myint;\nint main() { return 0; }" },
        { "union_rejected", "union U { int a; float b; };\nint main() { return 0; }" },
        { "enum_rejected", "enum E { A, B };\nint main() { return 0; }" },
        { "function_pointer_rejected", "int (*cb)(int);\nint main() { return 0; }" },
        { "variadic_rejected", "int vsum(int n, ...) { return 0; }\nint main() { return 0; }" },
        { "nested_function_rejected", "int main() { int inner(int x) { return x; } return inner(1); }" },
        { "undefined_goto_label", "int main() { goto nowhere; return 0; }" },
        { "bitfield_rejected", "struct B { int a : 3; };\nint main() { return 0; }" },
        { "unknown_typedef_type", "int main() { FILE *f; return 0; }" },
        { "garbage_top_level", "@@@ !!!" },
    };
    int nInvalid = (int)(sizeof(invalid) / sizeof(invalid[0]));
    for (int i = 0; i < nInvalid; i++) {
        CompileResult *r = nova_compile(invalid[i][1], NULL, 0);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s: fails", invalid[i][0]);
        check(nm, r->success == 0, r->success ? "expected failure but succeeded" : NULL);
        snprintf(nm, sizeof(nm), "%s: has diagnostics", invalid[i][0]);
        check(nm, r->diags->count > 0, "no diagnostics emitted");
        nova_compile_free(r);
    }

    /* ---- runtime errors ---- */
    {
        CompileResult *r = nova_compile("int main() { int a = 1; int b = a / 0; return 0; }", NULL, 0);
        int found = 0;
        for (int i = 0; i < r->diags->count; i++)
            if (strcmp(r->diags->items[i].level, "runtime") == 0 && strstr(r->diags->items[i].msg, "zero")) found = 1;
        check("div_by_zero: runtime diagnostic", found, NULL);
        nova_compile_free(r);
    }
    {
        CompileResult *r = nova_compile("int main() { int a[2]; a[5] = 1; return 0; }", NULL, 0);
        int found = 0;
        for (int i = 0; i < r->diags->count; i++)
            if (strcmp(r->diags->items[i].level, "runtime") == 0 && strstr(r->diags->items[i].msg, "bounds")) found = 1;
        check("oob_index: runtime diagnostic", found, NULL);
        nova_compile_free(r);
    }
    {
        CompileResult *r = nova_compile("int f(int n) { return n * f(n - 1); }\nint main() { return f(9); }", NULL, 0);
        int found = 0;
        for (int i = 0; i < r->diags->count; i++)
            if (strcmp(r->diags->items[i].level, "runtime") == 0) found = 1;
        check("deep_recursion: runtime diagnostic", found, NULL);
        nova_compile_free(r);
    }
    {
        CompileResult *r = nova_compile("int main() { while (1) { } return 0; }", NULL, 0);
        int found = 0;
        for (int i = 0; i < r->diags->count; i++)
            if (strcmp(r->diags->items[i].level, "runtime") == 0 && strstr(r->diags->items[i].msg, "step limit")) found = 1;
        check("infinite_loop: step limit diagnostic", found, NULL);
        nova_compile_free(r);
    }

    /* ---- scanf suspension ---- */
    {
        CompileResult *r1 = nova_compile("int main() { int n; scanf(\"%d\", &n); printf(\"%d\\n\", n); return 0; }", NULL, 0);
        check("scanf: suspends when no input", r1->vm && r1->vm->waitingForInput, NULL);
        nova_compile_free(r1);
        const char *inputs[] = { "42" };
        CompileResult *r2 = nova_compile("int main() { int n; scanf(\"%d\", &n); printf(\"%d\\n\", n); return 0; }", inputs, 1);
        check("scanf: resumes with input", r2->vm && !r2->vm->waitingForInput && strcmp(r2->vm->consoleOutput, "42\n") == 0, NULL);
        nova_compile_free(r2);
    }

    /* ---- determinism ---- */
    {
        const char *src = "int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { int a[3] = {1,2,3}; printf(\"%d %d\\n\", fact(5), a[2]); return 0; }";
        CompileResult *r1 = nova_compile(src, NULL, 0);
        CompileResult *r2 = nova_compile(src, NULL, 0);
        char *j1 = nova_to_json(r1);
        char *j2 = nova_to_json(r2);
        /* compare everything except compile_time_ms by comparing console + structure lengths */
        check("determinism: same console", strcmp(r1->vm->consoleOutput, r2->vm->consoleOutput) == 0, NULL);
        check("determinism: same tac count", r1->tac->count == r2->tac->count, NULL);
        check("determinism: same bytecode count", r1->bytecode->count == r2->bytecode->count, NULL);
        check("determinism: same vm steps", r1->vm->count == r2->vm->count, NULL);
        free(j1); free(j2);
        nova_compile_free(r1);
        nova_compile_free(r2);
    }

    /* ---- deterministic rand for a given seed ---- */
    {
        const char *src = "int main() { srand(42); printf(\"%d %d\\n\", rand(), rand()); return 0; }";
        CompileResult *r1 = nova_compile(src, NULL, 0);
        CompileResult *r2 = nova_compile(src, NULL, 0);
        check("rand: deterministic for seed",
              strcmp(r1->vm->consoleOutput, r2->vm->consoleOutput) == 0 && strlen(r1->vm->consoleOutput) > 0, NULL);
        nova_compile_free(r1);
        nova_compile_free(r2);
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

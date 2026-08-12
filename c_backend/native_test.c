/* Native backend test harness — mirrors tests/run_engine_test.mjs.
 * Exits non-zero if any assertion fails. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compile.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(int cond, const char* name) {
    if (cond) {
        g_passed++;
        printf("[PASS] %s\n", name);
    } else {
        g_failed++;
        printf("[FAIL] %s\n", name);
    }
}

static void check_console(const char* name, const char* code, const char* expected) {
    CompileResult* r = compile_source(code, NULL, 0);
    char label[256];
    snprintf(label, sizeof(label), "%s: success", name);
    if (!r->success) {
        check(0, label);
        for (int i = 0; i < r->diags->count; i++) {
            printf("       diag: %s: %s (line %d)\n", r->diags->items[i].level,
                   r->diags->items[i].msg, r->diags->items[i].line);
        }
        compile_result_free(r);
        return;
    }
    check(1, label);
    const char* out = r->vm ? r->vm->console_output : "";
    snprintf(label, sizeof(label), "%s: console == \"%s\"", name, expected);
    if (strcmp(out, expected) != 0) {
        g_failed++;
        printf("[FAIL] %s\n       got: \"%s\"\n", label, out);
    } else {
        g_passed++;
        printf("[PASS] %s\n", label);
    }
    compile_result_free(r);
}

static void check_invalid(const char* name, const char* code) {
    CompileResult* r = compile_source(code, NULL, 0);
    char label[256];
    snprintf(label, sizeof(label), "%s: fails", name);
    check(!r->success, label);
    snprintf(label, sizeof(label), "%s: has diagnostics", name);
    check(r->diags->count > 0, label);
    compile_result_free(r);
}

static int has_diag_matching(CompileResult* r, const char* level, const char* substr) {
    for (int i = 0; i < r->diags->count; i++) {
        if (strcmp(r->diags->items[i].level, level) == 0 &&
            strstr(r->diags->items[i].msg, substr)) return 1;
    }
    return 0;
}

int main(void) {
    /* ---- valid programs (exact console output) ---- */
    check_console("hello_world",
        "#include <stdio.h>\nint main() { printf(\"Hello, World!\\n\"); return 0; }",
        "Hello, World!\n");
    check_console("arithmetic_precedence",
        "int main() { int x = 2 + 3 * 4; printf(\"%d\\n\", x); return 0; }",
        "14\n");
    check_console("parentheses",
        "int main() { int x = (2 + 3) * 4; printf(\"%d\\n\", x); return 0; }",
        "20\n");
    check_console("int_division_truncates",
        "int main() { printf(\"%d %d\\n\", 7 / 2, -7 / 2); return 0; }",
        "3 -3\n");
    check_console("modulo",
        "int main() { printf(\"%d %d\\n\", 7 % 3, -7 % 3); return 0; }",
        "1 -1\n");
    check_console("comparisons",
        "int main() { printf(\"%d %d %d %d %d %d\\n\", 1 < 2, 2 < 1, 2 <= 2, 3 >= 4, 5 == 5, 5 != 5); return 0; }",
        "1 0 1 0 1 0\n");
    check_console("logical",
        "int main() { int a = 1, b = 0; printf(\"%d %d %d %d\\n\", a && b, a || b, !a, !!a); return 0; }",
        "0 1 0 1\n");
    check_console("if_else",
        "int main() { int x = 5; if (x > 3) printf(\"big\\n\"); else printf(\"small\\n\"); return 0; }",
        "big\n");
    check_console("while_factorial",
        "int main() { int n = 5, f = 1; while (n > 1) { f = f * n; n = n - 1; } printf(\"%d\\n\", f); return 0; }",
        "120\n");
    check_console("for_sum",
        "int main() { int s = 0; for (int i = 1; i <= 10; i = i + 1) s = s + i; printf(\"%d\\n\", s); return 0; }",
        "55\n");
    check_console("for_postfix",
        "int main() { int s = 0; for (int i = 0; i < 5; i++) s += i; printf(\"%d\\n\", s); return 0; }",
        "10\n");
    check_console("break_continue",
        "int main() { int s = 0; for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; s += i; } printf(\"%d\\n\", s); return 0; }",
        "18\n");
    check_console("compound_assignments",
        "int main() { int x = 10; x += 5; x -= 2; x *= 3; x /= 2; x %= 4; printf(\"%d\\n\", x); return 0; }",
        "3\n");
    check_console("function_call",
        "int add(int a, int b) { return a + b; }\nint main() { printf(\"%d\\n\", add(2, 3)); return 0; }",
        "5\n");
    check_console("recursion_factorial",
        "int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { printf(\"%d\\n\", fact(6)); return 0; }",
        "720\n");
    check_console("mutual_globals",
        "int g = 21;\nint twice() { return g * 2; }\nint main() { printf(\"%d\\n\", twice()); return 0; }",
        "42\n");
    check_console("array_index_sum",
        "int main() { int a[4] = {10, 20, 30, 40}; int s = 0; for (int i = 0; i < 4; i++) s += a[i]; printf(\"%d\\n\", s); return 0; }",
        "100\n");
    check_console("array_write",
        "int main() { int a[3]; a[0] = 1; a[1] = 2; a[2] = a[0] + a[1]; printf(\"%d\\n\", a[2]); return 0; }",
        "3\n");
    check_console("pointers",
        "int main() { int x = 5; int *p = &x; *p = *p + 2; printf(\"%d\\n\", x); return 0; }",
        "7\n");
    check_console("pointer_params_swap",
        "void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }\nint main() { int x = 1, y = 2; swap(&x, &y); printf(\"%d %d\\n\", x, y); return 0; }",
        "2 1\n");
    check_console("struct_members",
        "struct P { int x; float y; };\nint main() { struct P p; p.x = 3; p.y = 1.5; printf(\"%d %.2f\\n\", p.x, p.y); return 0; }",
        "3 1.50\n");
    check_console("float_arithmetic",
        "int main() { float f = 7.0 / 2.0; printf(\"%.2f\\n\", f); return 0; }",
        "3.50\n");
    check_console("char_literal",
        "int main() { char c = 'A'; printf(\"%c %d\\n\", c, c); return 0; }",
        "A 65\n");
    check_console("bubble_sort",
        "#include <stdio.h>\n"
        "void swap(int *a, int *b) { int temp = *a; *a = *b; *b = temp; }\n"
        "int main() {\n"
        "    int arr[5] = {64, 34, 25, 12, 22};\n"
        "    int n = 5;\n"
        "    for (int i = 0; i < n - 1; i++) {\n"
        "        for (int j = 0; j < n - i - 1; j++) {\n"
        "            if (arr[j] > arr[j + 1]) { swap(&arr[j], &arr[j + 1]); }\n"
        "        }\n"
        "    }\n"
        "    for (int i = 0; i < n; i++) printf(\"%d \", arr[i]);\n"
        "    printf(\"\\n\");\n"
        "    return 0;\n"
        "}",
        "12 22 25 34 64 \n");

    /* ---- scanf protocol ---- */
    {
        const char* code = "int main() { int n; scanf(\"%d\", &n); printf(\"%d\\n\", n); return 0; }";
        CompileResult* r1 = compile_source(code, NULL, 0);
        check(r1->vm && r1->vm->waiting_for_input && strlen(r1->vm->input_prompt) > 0,
              "scanf: suspends when no input");
        compile_result_free(r1);
        const char* in1[] = { "17" };
        CompileResult* r2 = compile_source(code, in1, 1);
        check(r2->vm && !r2->vm->waiting_for_input && strcmp(r2->vm->console_output, "17\n") == 0,
              "scanf: resumes with input");
        compile_result_free(r2);
    }

    /* ---- invalid programs ---- */
    check_invalid("empty_source", "");
    check_invalid("missing_main", "int foo() { return 1; }");
    check_invalid("undefined_identifier", "int main() { int x = y + 1; return 0; }");
    check_invalid("missing_semicolon", "int main() { int x = 5 return 0; }");
    check_invalid("unbalanced_brace", "int main() { { return 0; }");
    check_invalid("unterminated_string", "int main() { printf(\"abc); return 0; }");
    check_invalid("wrong_arg_count", "int f(int a) { return a; }\nint main() { return f(1, 2); }");
    check_invalid("break_outside_loop", "int main() { break; return 0; }");
    check_invalid("unknown_struct_field", "struct S { int x; };\nint main() { struct S s; s.zzz = 1; return 0; }");
    check_invalid("duplicate_global", "int g; int g;\nint main() { return 0; }");

    /* ---- runtime errors ---- */
    {
        CompileResult* r = compile_source("int main() { int a = 1; int b = a / 0; return 0; }", NULL, 0);
        check(has_diag_matching(r, "runtime", "zero"), "div_by_zero: runtime diagnostic");
        compile_result_free(r);
    }
    {
        CompileResult* r = compile_source("int main() { int a[2]; a[5] = 1; return 0; }", NULL, 0);
        check(has_diag_matching(r, "runtime", "bounds"), "oob_index: runtime diagnostic");
        compile_result_free(r);
    }
    {
        CompileResult* r = compile_source("int fact(int n) { return n * fact(n - 1); }\nint main() { return fact(9); }", NULL, 0);
        check(has_diag_matching(r, "runtime", ""), "deep_recursion: diagnosed, no crash");
        compile_result_free(r);
    }
    {
        CompileResult* r = compile_source("int main() { while (1) { } return 0; }", NULL, 0);
        check(has_diag_matching(r, "runtime", "step limit"), "infinite_loop: step limit diagnostic");
        compile_result_free(r);
    }

    /* ---- optimizer sanity ---- */
    {
        CompileResult* r = compile_source("int main() { int x = 2 * 3 + 4; printf(\"%d\\n\", x); return 0; }", NULL, 0);
        check(r->vm && strcmp(r->vm->console_output, "10\n") == 0, "optimizer: folding preserves semantics");
        check(r->metrics.constant_fold >= 1, "optimizer: fold counted");
        check(r->opt_tac->count <= r->tac_gen->instrs->count, "optimizer: instruction count never increases");
        compile_result_free(r);
    }

    /* ---- determinism ---- */
    {
        const char* src = "int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\n"
                          "int main() { int a[3] = {1,2,3}; printf(\"%d %d\\n\", fact(5), a[2]); return 0; }";
        CompileResult* r1 = compile_source(src, NULL, 0);
        CompileResult* r2 = compile_source(src, NULL, 0);
        char* j1 = serialize_result_json(r1);
        char* j2 = serialize_result_json(r2);
        /* compile_time_ms legitimately differs; compare everything else via console + trace */
        check(r1->vm && r2->vm && strcmp(r1->vm->console_output, r2->vm->console_output) == 0,
              "determinism: identical console across runs");
        check(r1->vm && r2->vm && r1->vm->count == r2->vm->count,
              "determinism: identical trace length across runs");
        free(j1);
        free(j2);
        compile_result_free(r1);
        compile_result_free(r2);
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
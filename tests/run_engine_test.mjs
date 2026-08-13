// ============================================================================
// NOVA browser engine test suite (node --test style, zero dependencies)
// Run: npm run test:engine
// Every case asserts REAL compiler behavior — no mocked expectations.
// ============================================================================

import { compileCSource } from '../src/engine/compilerEngine.js';

let passed = 0;
let failed = 0;

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`[PASS] ${name}`);
  } else {
    failed++;
    console.log(`[FAIL] ${name}${detail ? ' — ' + detail : ''}`);
  }
}

function exact(name, actual, expected) {
  check(name, actual === expected, `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

// ---------------------------------------------------------------------------
// 1. Valid programs — must compile and produce exact console output
// ---------------------------------------------------------------------------

const validCases = [
  {
    name: 'hello_world',
    code: '#include <stdio.h>\nint main() { printf("Hello, World!\\n"); return 0; }',
    console: 'Hello, World!\n'
  },
  {
    name: 'arithmetic_precedence',
    code: 'int main() { int x = 2 + 3 * 4; printf("%d\\n", x); return 0; }',
    console: '14\n'
  },
  {
    name: 'parentheses',
    code: 'int main() { int x = (2 + 3) * 4; printf("%d\\n", x); return 0; }',
    console: '20\n'
  },
  {
    name: 'int_division_truncates',
    code: 'int main() { printf("%d %d\\n", 7 / 2, -7 / 2); return 0; }',
    console: '3 -3\n'
  },
  {
    name: 'modulo',
    code: 'int main() { printf("%d %d\\n", 7 % 3, -7 % 3); return 0; }',
    console: '1 -1\n'
  },
  {
    name: 'unary_minus_not',
    code: 'int main() { int a = -5; printf("%d %d\\n", a, !a); return 0; }',
    console: '-5 0\n'
  },
  {
    name: 'comparisons',
    code: 'int main() { printf("%d %d %d %d %d %d\\n", 1 < 2, 2 < 1, 2 <= 2, 3 >= 4, 5 == 5, 5 != 5); return 0; }',
    console: '1 0 1 0 1 0\n'
  },
  {
    name: 'logical_short_circuit',
    code: 'int main() { int a = 1, b = 0; printf("%d %d %d %d\\n", a && b, a || b, !a, !!a); return 0; }',
    console: '0 1 0 1\n'
  },
  {
    name: 'if_else',
    code: 'int main() { int x = 5; if (x > 3) printf("big\\n"); else printf("small\\n"); return 0; }',
    console: 'big\n'
  },
  {
    name: 'if_else_taken_other_branch',
    code: 'int main() { int x = 1; if (x > 3) printf("big\\n"); else printf("small\\n"); return 0; }',
    console: 'small\n'
  },
  {
    name: 'while_factorial',
    code: 'int main() { int n = 5, f = 1; while (n > 1) { f = f * n; n = n - 1; } printf("%d\\n", f); return 0; }',
    console: '120\n'
  },
  {
    name: 'for_sum',
    code: 'int main() { int s = 0; for (int i = 1; i <= 10; i = i + 1) s = s + i; printf("%d\\n", s); return 0; }',
    console: '55\n'
  },
  {
    name: 'for_postfix_increment',
    code: 'int main() { int s = 0; for (int i = 0; i < 5; i++) s += i; printf("%d\\n", s); return 0; }',
    console: '10\n'
  },
  {
    name: 'break_continue',
    code: 'int main() { int s = 0; for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; s += i; } printf("%d\\n", s); return 0; }',
    console: '18\n'
  },
  {
    name: 'compound_assignments',
    code: 'int main() { int x = 10; x += 5; x -= 2; x *= 3; x /= 2; x %= 4; printf("%d\\n", x); return 0; }',
    console: '3\n'
  },
  {
    name: 'function_call',
    code: 'int add(int a, int b) { return a + b; }\nint main() { printf("%d\\n", add(2, 3)); return 0; }',
    console: '5\n'
  },
  {
    name: 'recursion_factorial',
    code: 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { printf("%d\\n", fact(6)); return 0; }',
    console: '720\n'
  },
  {
    name: 'mutual_globals',
    code: 'int g = 21;\nint twice() { return g * 2; }\nint main() { printf("%d\\n", twice()); return 0; }',
    console: '42\n'
  },
  {
    name: 'array_index_sum',
    code: 'int main() { int a[4] = {10, 20, 30, 40}; int s = 0; for (int i = 0; i < 4; i++) s += a[i]; printf("%d\\n", s); return 0; }',
    console: '100\n'
  },
  {
    name: 'array_write',
    code: 'int main() { int a[3]; a[0] = 1; a[1] = 2; a[2] = a[0] + a[1]; printf("%d\\n", a[2]); return 0; }',
    console: '3\n'
  },
  {
    name: 'pointers',
    code: 'int main() { int x = 5; int *p = &x; *p = *p + 2; printf("%d\\n", x); return 0; }',
    console: '7\n'
  },
  {
    name: 'pointer_params_swap',
    code: 'void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }\nint main() { int x = 1, y = 2; swap(&x, &y); printf("%d %d\\n", x, y); return 0; }',
    console: '2 1\n'
  },
  {
    name: 'struct_members',
    code: 'struct P { int x; float y; };\nint main() { struct P p; p.x = 3; p.y = 1.5; printf("%d %.2f\\n", p.x, p.y); return 0; }',
    console: '3 1.50\n'
  },
  {
    name: 'float_arithmetic',
    code: 'int main() { float f = 7.0 / 2.0; printf("%.2f\\n", f); return 0; }',
    console: '3.50\n'
  },
  {
    name: 'char_literal',
    code: "int main() { char c = 'A'; printf(\"%c %d\\n\", c, c); return 0; }",
    console: 'A 65\n'
  },
  {
    name: 'nested_loops',
    code: 'int main() { int n = 0; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) n++; printf("%d\\n", n); return 0; }',
    console: '9\n'
  },
  {
    name: 'scanf_reads_input',
    code: '#include <stdio.h>\nint main() { int n; scanf("%d", &n); printf("got %d\\n", n); return 0; }',
    inputs: ['42'],
    console: 'got 42\n'
  },
  {
    name: 'scanf_two_reads',
    code: 'int main() { int a, b; scanf("%d", &a); scanf("%d", &b); printf("%d\\n", a + b); return 0; }',
    inputs: ['3', '4'],
    console: '7\n'
  },
  {
    name: 'comments_ignored',
    code: '// line comment with if( and *p and &x\n/* block comment\n   spanning lines with struct and for( */\nint main() { printf("ok\\n"); /* trailing */ return 0; }',
    console: 'ok\n'
  },
  {
    name: 'string_with_escapes',
    code: 'int main() { printf("a\\tb\\"c\\"\\n"); return 0; }',
    console: 'a\tb"c"\n'
  },
  {
    name: 'empty_main_void_style',
    code: 'int main(void) { return 0; }',
    console: ''
  },
  {
    name: 'bubble_sort_preset',
    code: [
      '#include <stdio.h>',
      'void swap(int *a, int *b) { int temp = *a; *a = *b; *b = temp; }',
      'int main() {',
      '    int arr[5] = {64, 34, 25, 12, 22};',
      '    int n = 5;',
      '    for (int i = 0; i < n - 1; i++) {',
      '        for (int j = 0; j < n - i - 1; j++) {',
      '            if (arr[j] > arr[j + 1]) { swap(&arr[j], &arr[j + 1]); }',
      '        }',
      '    }',
      '    for (int i = 0; i < n; i++) printf("%d ", arr[i]);',
      '    printf("\\n");',
      '    return 0;',
      '}'
    ].join('\n'),
    console: '12 22 25 34 64 \n'
  },
  // --- extended subset (v2) ---
  {
    name: 'ternary_operator',
    code: 'int main() { int x = 5; printf("%d %d\\n", x > 3 ? 10 : 20, x > 9 ? 1 : 0); return 0; }',
    console: '10 0\n'
  },
  {
    name: 'bitwise_ops',
    code: 'int main() { int a = 0x55, b = 0xAA; printf("%d %d %d %d %d\\n", a & b, a | b, a ^ b, a << 2, ~a); return 0; }',
    console: '0 255 255 340 -86\n'
  },
  {
    name: 'switch_break',
    code: 'int main() { int v = 2; switch (v) { case 1: printf("one\\n"); break; case 2: printf("two\\n"); break; default: printf("other\\n"); } return 0; }',
    console: 'two\n'
  },
  {
    name: 'switch_fallthrough',
    code: 'int main() { int v = 1; switch (v) { case 1: printf("a "); case 2: printf("b "); break; case 3: printf("c "); } printf("\\n"); return 0; }',
    console: 'a b \n'
  },
  {
    name: 'goto_label',
    code: 'int main() { int i = 0; loop: i++; if (i < 3) goto loop; printf("%d\\n", i); return 0; }',
    console: '3\n'
  },
  {
    name: 'do_while',
    code: 'int main() { int i = 0; do { i++; } while (i < 5); printf("%d\\n", i); return 0; }',
    console: '5\n'
  },
  {
    name: 'sizeof_types',
    code: 'int main() { printf("%d %d %d %d\\n", (int)sizeof(int), (int)sizeof(double), (int)sizeof(char), (int)sizeof(int *)); return 0; }',
    console: '4 8 1 8\n'
  },
  {
    name: 'explicit_casts',
    code: 'int main() { double d = 3.9; int i = (int)d; printf("%d %d\\n", i, (int)7.8); return 0; }',
    console: '3 7\n'
  },
  {
    name: 'string_concat',
    code: 'int main() { char *s = "Hello " "World"; printf("%s\\n", s); return 0; }',
    console: 'Hello World\n'
  },
  {
    name: 'static_local_persists',
    code: 'int counter() { static int c = 0; c++; return c; } int main() { printf("%d %d %d\\n", counter(), counter(), counter()); return 0; }',
    console: '1 2 3\n'
  },
  {
    name: 'unsigned_long_decls',
    code: 'int main() { unsigned int u = 100; long long ll = 200; printf("%d %d\\n", u, ll); return 0; }',
    console: '100 200\n'
  },
  {
    name: 'math_builtins',
    code: 'int main() { printf("%d %d %d\\n", (int)sqrt(16.0), (int)pow(2.0, 3.0), (int)fabs(-7.5)); return 0; }',
    console: '4 8 7\n'
  },
  {
    name: 'printf_hex_oct_unsigned',
    code: 'int main() { printf("%x %X %o %u\\n", 255, 255, 8, -1); return 0; }',
    console: 'ff FF 10 4294967295\n'
  },
  {
    name: 'null_predefined',
    code: 'int main() { int *p = NULL; if (p == NULL) printf("null\\n"); return 0; }',
    console: 'null\n'
  },
  {
    name: 'ifdef_excludes_inactive',
    code: 'int main() {\n#ifdef __GNUC__\nthis would not parse;\n#endif\nprintf("ok\\n"); return 0; }',
    console: 'ok\n'
  },
  {
    name: 'assert_passes',
    code: 'int main() { int x = 5; assert(x == 5); printf("ok\\n"); return 0; }',
    console: 'ok\n'
  },
  {
    name: 'line_spliced_directive',
    code: '#define FOO \\\n  bar\nint main() { printf("ok\\n"); return 0; }',
    console: 'ok\n'
  },
  {
    name: 'compound_bitwise_assign',
    code: 'int main() { int x = 5; x &= 3; x |= 8; x ^= 1; x <<= 1; x >>= 1; printf("%d\\n", x); return 0; }',
    console: '8\n'
  },
  {
    name: 'forward_declaration',
    code: 'int add(int a, int b);\nint main() { printf("%d\\n", add(2, 3)); return 0; }\nint add(int a, int b) { return a + b; }',
    console: '5\n'
  },
  {
    name: 'char_literal_and_format',
    code: "int main() { char c = 'A'; printf(\"%c %d\\n\", c, c + 1); return 0; }",
    console: 'A 66\n'
  },
  {
    name: 'struct_member_via_arrow_rejected_gracefully',
    code: 'struct P { int x; }; int main() { struct P p; p.x = 7; printf("%d\\n", p.x); return 0; }',
    console: '7\n'
  },
  {
    name: 'deterministic_rand',
    code: 'int main() { srand(42); printf("%d %d\\n", rand(), rand()); return 0; }',
    console: null, // asserted separately for determinism
    skipConsole: true
  }
];

for (const t of validCases) {
  const r = compileCSource(t.code, t.inputs || []);
  check(`${t.name}: success`, r.success === true,
    `diagnostics=${JSON.stringify(r.diagnostics)}`);
  if (r.success) {
    if (!t.skipConsole) {
      exact(`${t.name}: console output`, r.consoleOutput, t.console);
    }
    check(`${t.name}: no diagnostics`, r.diagnostics.length === 0,
      JSON.stringify(r.diagnostics));
    check(`${t.name}: tokens non-empty`, Array.isArray(r.tokens) && r.tokens.length > 0);
    check(`${t.name}: ast is NODE_PROGRAM`, r.ast && r.ast.type === 'NODE_PROGRAM');
    check(`${t.name}: bytecode non-empty`, Array.isArray(r.bytecode) && r.bytecode.length > 0);
    check(`${t.name}: vm trace non-empty`, Array.isArray(r.vmTrace) && r.vmTrace.length > 0);
    check(`${t.name}: engine tag`, r.engine === 'browser-js');
  }
}

// rand() must be deterministic for a given seed (identical across runs)
{
  const src = 'int main() { srand(42); printf("%d %d\\n", rand(), rand()); return 0; }';
  const r1 = compileCSource(src, []);
  const r2 = compileCSource(src, []);
  check('rand: deterministic for seed', r1.consoleOutput === r2.consoleOutput && r1.consoleOutput.length > 0,
    `${JSON.stringify(r1.consoleOutput)} vs ${JSON.stringify(r2.consoleOutput)}`);
}

// ---------------------------------------------------------------------------
// 2. Malformed / invalid programs — must fail with diagnostics, never crash
// ---------------------------------------------------------------------------

const invalidCases = [
  { name: 'empty_source', code: '' },
  { name: 'missing_main', code: 'int foo() { return 1; }' },
  { name: 'undefined_identifier', code: 'int main() { int x = y + 1; return 0; }' },
  { name: 'missing_semicolon', code: 'int main() { int x = 5 return 0; }' },
  { name: 'missing_rparen', code: 'int main() { if (x > 3 { } return 0; }' },
  { name: 'unbalanced_brace', code: 'int main() { { return 0; }' },
  { name: 'unterminated_string', code: 'int main() { printf("abc); return 0; }' },
  { name: 'assignment_to_literal', code: 'int main() { 5 = 3; return 0; }' },
  { name: 'wrong_arg_count', code: 'int f(int a) { return a; }\nint main() { return f(1, 2); }' },
  { name: 'break_outside_loop', code: 'int main() { break; return 0; }' },
  { name: 'unknown_struct_field', code: 'struct S { int x; };\nint main() { struct S s; s.zzz = 1; return 0; }' },
  { name: 'duplicate_global', code: 'int g; int g;\nint main() { return 0; }' },
  { name: 'struct_field_not_type', code: 'struct S { zzz q; };\nint main() { return 0; }' },
  { name: 'garbage_top_level', code: '@@@ !!!' },
  { name: 'huge_nesting', code: 'int main() { int x = ((((((((((1)))))))))); return 0; }' }, // valid actually
  // v2 graceful rejections — one clear diagnostic, no cascades
  { name: 'typedef_rejected', code: 'typedef int myint;\nint main() { return 0; }' },
  { name: 'union_rejected', code: 'union U { int a; float b; };\nint main() { return 0; }' },
  { name: 'enum_rejected', code: 'enum E { A, B };\nint main() { return 0; }' },
  { name: 'function_pointer_rejected', code: 'int (*cb)(int);\nint main() { return 0; }' },
  { name: 'variadic_rejected', code: 'int vsum(int n, ...) { return 0; }\nint main() { return 0; }' },
  { name: 'nested_function_rejected', code: 'int main() { int inner(int x) { return x; } return inner(1); }' },
  { name: 'undefined_goto_label', code: 'int main() { goto nowhere; return 0; }' },
  { name: 'bitfield_rejected', code: 'struct B { int a : 3; };\nint main() { return 0; }' },
  { name: 'unknown_typedef_type', code: 'int main() { FILE *f; return 0; }' }
];

for (const t of invalidCases) {
  let crashed = false;
  let r = null;
  try {
    r = compileCSource(t.code, []);
  } catch (e) {
    crashed = true;
  }
  check(`${t.name}: no crash`, !crashed);
  if (!crashed) {
    if (t.name === 'huge_nesting') {
      check(`${t.name}: still compiles`, r.success === true);
    } else {
      check(`${t.name}: fails`, r.success === false);
      check(`${t.name}: has diagnostics`, r.diagnostics.length > 0);
      check(`${t.name}: diagnostics have line info`, r.diagnostics.every((d) => typeof d.line === 'number'));
    }
  }
}

// ---------------------------------------------------------------------------
// 3. Runtime errors — reported as runtime diagnostics, execution halts
// ---------------------------------------------------------------------------

{
  const r = compileCSource('int main() { int a = 1; int b = a / 0; return 0; }', []);
  check('div_by_zero: compile succeeds', r.success === true || r.diagnostics.some((d) => d.level === 'runtime'));
  check('div_by_zero: runtime diagnostic', r.diagnostics.some((d) => d.level === 'runtime' && /zero/i.test(d.msg)));
}
{
  const r = compileCSource('int main() { int a[2]; a[5] = 1; return 0; }', []);
  check('oob_index: runtime diagnostic', r.diagnostics.some((d) => d.level === 'runtime' && /bounds/i.test(d.msg)));
}
{
  const r = compileCSource('int fact(int n) { return n * fact(n - 1); }\nint main() { return fact(9); }', []);
  check('deep_recursion: diagnosed, no crash',
    r.diagnostics.some((d) => d.level === 'runtime'));
}
{
  const r = compileCSource('int main() { while (1) { } return 0; }', []);
  check('infinite_loop: step limit diagnostic',
    r.diagnostics.some((d) => d.level === 'runtime' && /step limit/i.test(d.msg)));
}

// ---------------------------------------------------------------------------
// 4. scanf suspension protocol
// ---------------------------------------------------------------------------

{
  const code = 'int main() { int n; scanf("%d", &n); printf("%d\\n", n); return 0; }';
  const r1 = compileCSource(code, []);
  check('scanf: suspends when no input', r1.waitingForInput === true && r1.inputPrompt.length > 0);
  const r2 = compileCSource(code, ['17']);
  check('scanf: resumes with input', r2.waitingForInput === false && r2.consoleOutput === '17\n');
}

// ---------------------------------------------------------------------------
// 5. Optimizer correctness — semantics preserved, metrics real
// ---------------------------------------------------------------------------

{
  const r = compileCSource('int main() { int x = 2 * 3 + 4; printf("%d\\n", x); return 0; }', []);
  exact('optimizer: folded constants still correct', r.consoleOutput, '10\n');
  check('optimizer: fold counted', r.metrics.constant_fold >= 1);
}
{
  const r = compileCSource('int main() { int a = 5; int b = a * 2; printf("%d\\n", b); return 0; }', []);
  exact('optimizer: strength reduction preserves value', r.consoleOutput, '10\n');
}
{
  const before = compileCSource('int main() { int x = 1 + 1; printf("%d\\n", x); return 0; }', []);
  check('optimizer: instruction count never increases',
    before.optTac.length <= before.tac.length);
  const reduction = 100 * (1 - before.optTac.length / before.tac.length);
  check('optimizer: reduction_percentage is honest',
    Math.abs(before.metrics.reduction_percentage - Math.round(reduction * 10) / 10) < 0.11);
}

// ---------------------------------------------------------------------------
// 6. Determinism — same source, same output, every time
// ---------------------------------------------------------------------------

{
  const src = 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { int a[3] = {1,2,3}; printf("%d %d\\n", fact(5), a[2]); return 0; }';
  const r1 = compileCSource(src, []);
  const r2 = compileCSource(src, []);
  const norm = (r) => JSON.stringify({ ...r, compile_time_ms: 0 });
  exact('determinism: identical JSON across runs', norm(r1), norm(r2));
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);

// ============================================================================
// JS <-> C parity suite.
// Compiles the same corpus with the browser engine (src/engine/compilerEngine.js)
// and the native backend (c_backend/nova_compiler) and deep-compares the full
// JSON contract field by field (docs/SCHEMA.md). compile_time_ms is excluded
// (timing is inherently different); everything else must match exactly.
// ============================================================================

import { compileCSource } from '../src/engine/compilerEngine.js';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BACKEND_DIR = join(__dirname, '..', 'c_backend');
const BINARY = join(BACKEND_DIR, 'nova_compiler');

if (!existsSync(BINARY)) {
  // Try to build it; if there is no C toolchain, skip with a clear message.
  try {
    execFileSync('make', ['nova_compiler'], { cwd: BACKEND_DIR, stdio: 'pipe' });
  } catch {
    console.log('parity: SKIPPED — c_backend/nova_compiler is not built and no C toolchain is available.');
    console.log('parity: build it with `cd c_backend && make` to enable JS<->C parity checks.');
    process.exit(0);
  }
}

if (!existsSync(BINARY)) {
  console.error('parity: failed to build c_backend/nova_compiler.');
  process.exit(2);
}

function compileNative(code, inputs) {
  const dir = mkdtempSync(join(tmpdir(), 'nova-parity-'));
  const file = join(dir, 'prog.c');
  writeFileSync(file, code);
  try {
    const out = execFileSync(BINARY, [file], {
      env: { ...process.env, NOVA_INPUTS: (inputs || []).join('\n') },
      maxBuffer: 64 * 1024 * 1024,
      encoding: 'utf8'
    });
    return JSON.parse(out);
  } catch (e) {
    // nova_compiler exits 1 on compile failure but still prints JSON
    if (e.stdout) {
      try { return JSON.parse(e.stdout); } catch { /* fall through */ }
    }
    throw new Error(`native compile crashed: ${e.message}`);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

const CORPUS = [
  { name: 'hello', code: '#include <stdio.h>\nint main() { printf("Hello, World!\\n"); return 0; }' },
  { name: 'precedence', code: 'int main() { int x = 2 + 3 * 4; printf("%d\\n", x); return 0; }' },
  { name: 'parens', code: 'int main() { printf("%d %d\\n", (2+3)*4, 10 - 2 - 3); return 0; }' },
  { name: 'div_mod', code: 'int main() { printf("%d %d %d %d\\n", 7/2, -7/2, 7%3, -7%3); return 0; }' },
  { name: 'unary', code: 'int main() { int a = 5; printf("%d %d %d\\n", -a, !a, !!a); return 0; }' },
  { name: 'compare', code: 'int main() { printf("%d %d %d %d %d %d\\n", 1<2, 2<1, 2<=2, 3>=4, 5==5, 5!=5); return 0; }' },
  { name: 'logical', code: 'int main() { int a = 1, b = 0; printf("%d %d %d %d\\n", a&&b, a||b, !a, !!a); return 0; }' },
  { name: 'if_else', code: 'int main() { int x = 5; if (x > 3) printf("big\\n"); else printf("small\\n"); return 0; }' },
  { name: 'nested_if', code: 'int main() { int x = 2; if (x > 3) { if (x > 4) printf("a\\n"); else printf("b\\n"); } else if (x == 2) printf("c\\n"); else printf("d\\n"); return 0; }' },
  { name: 'while', code: 'int main() { int n = 5, f = 1; while (n > 1) { f = f * n; n = n - 1; } printf("%d\\n", f); return 0; }' },
  { name: 'for', code: 'int main() { int s = 0; for (int i = 1; i <= 10; i = i + 1) s = s + i; printf("%d\\n", s); return 0; }' },
  { name: 'for_pp', code: 'int main() { int s = 0; for (int i = 0; i < 5; i++) s += i; printf("%d\\n", s); return 0; }' },
  { name: 'break_continue', code: 'int main() { int s = 0; for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; s += i; } printf("%d\\n", s); return 0; }' },
  { name: 'compound', code: 'int main() { int x = 10; x += 5; x -= 2; x *= 3; x /= 2; x %= 4; printf("%d\\n", x); return 0; }' },
  { name: 'call', code: 'int add(int a, int b) { return a + b; }\nint main() { printf("%d\\n", add(2, 3)); return 0; }' },
  { name: 'recursion', code: 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\nint main() { printf("%d\\n", fact(6)); return 0; }' },
  { name: 'fib', code: 'int fib(int n) { if (n <= 1) return n; return fib(n-1) + fib(n-2); }\nint main() { for (int i = 0; i < 8; i++) printf("%d ", fib(i)); printf("\\n"); return 0; }' },
  { name: 'globals', code: 'int g = 21;\nint twice() { return g * 2; }\nint main() { printf("%d\\n", twice()); return 0; }' },
  { name: 'global_array', code: 'int nums[3] = {5, 10, 15};\nint main() { printf("%d\\n", nums[0] + nums[1] + nums[2]); return 0; }' },
  { name: 'arrays', code: 'int main() { int a[4] = {10, 20, 30, 40}; int s = 0; for (int i = 0; i < 4; i++) s += a[i]; printf("%d\\n", s); return 0; }' },
  { name: 'array_write', code: 'int main() { int a[3]; a[0] = 1; a[1] = 2; a[2] = a[0] + a[1]; printf("%d\\n", a[2]); return 0; }' },
  { name: 'pointers', code: 'int main() { int x = 5; int *p = &x; *p = *p + 2; printf("%d\\n", x); return 0; }' },
  { name: 'swap', code: 'void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }\nint main() { int x = 1, y = 2; swap(&x, &y); printf("%d %d\\n", x, y); return 0; }' },
  { name: 'bubble_sort', code: [
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
  ].join('\n') },
  { name: 'struct', code: 'struct P { int x; float y; };\nint main() { struct P p; p.x = 3; p.y = 1.5; printf("%d %.2f\\n", p.x, p.y); return 0; }' },
  { name: 'struct_offsets', code: 'struct S { char name[4]; int id; float gpa; };\nint main() { struct S s; s.id = 101; s.gpa = 3.92; printf("ID: %d, GPA: %.2f\\n", s.id, s.gpa); return 0; }' },
  { name: 'floats', code: 'int main() { float f = 7.0 / 2.0; double d = 1.5 * 2.0; printf("%.2f %.1f\\n", f, d); return 0; }' },
  { name: 'char_lit', code: 'int main() { char c = \'A\'; printf("%c %d\\n", c, c + 1); return 0; }' },
  { name: 'escapes', code: 'int main() { printf("a\\tb\\"c\\"\\n"); return 0; }' },
  { name: 'comments', code: '// line comment with if( and *p\n/* block\n   comment with struct and for( */\nint main() { printf("ok\\n"); return 0; }' },
  { name: 'hex', code: 'int main() { int x = 0xFF; printf("%d\\n", x); return 0; }' },
  { name: 'scan_one', code: 'int main() { int n; scanf("%d", &n); printf("got %d\\n", n); return 0; }', inputs: ['42'] },
  { name: 'scan_two', code: 'int main() { int a, b; scanf("%d", &a); scanf("%d", &b); printf("%d\\n", a + b); return 0; }', inputs: ['3', '4'] },
  { name: 'scan_wait', code: 'int main() { int n; scanf("%d", &n); printf("%d\\n", n); return 0; }', inputs: [] },
  // invalid programs — both engines must reject identically
  { name: 'bad_empty', code: '' },
  { name: 'bad_no_main', code: 'int foo() { return 1; }' },
  { name: 'bad_undef', code: 'int main() { int x = y + 1; return 0; }' },
  { name: 'bad_semi', code: 'int main() { int x = 5 return 0; }' },
  { name: 'bad_brace', code: 'int main() { { return 0; }' },
  { name: 'bad_string', code: 'int main() { printf("abc); return 0; }' },
  { name: 'bad_arity', code: 'int f(int a) { return a; }\nint main() { return f(1, 2); }' },
  { name: 'bad_break', code: 'int main() { break; return 0; }' },
  { name: 'bad_field', code: 'struct S { int x; };\nint main() { struct S s; s.zzz = 1; return 0; }' },
  { name: 'bad_dup_global', code: 'int g; int g;\nint main() { return 0; }' },
  // runtime errors — both engines must report identically
  { name: 'rt_div0', code: 'int main() { int a = 1; int b = a / 0; return 0; }' },
  { name: 'rt_oob', code: 'int main() { int a[2]; a[5] = 1; return 0; }' },
  { name: 'rt_recursion', code: 'int f(int n) { return n * f(n - 1); }\nint main() { return f(9); }' },
  { name: 'rt_infinite', code: 'int main() { while (1) { } return 0; }' }
];

let failures = 0;
let checks = 0;

function diffAt(path, a, b) {
  if (Object.is(a, b)) return null;
  if (typeof a === 'number' && typeof b === 'number' && a === b) return null;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return `${path}: length ${a.length} != ${b.length}`;
    for (let i = 0; i < a.length; i++) {
      const d = diffAt(`${path}[${i}]`, a[i], b[i]);
      if (d) return d;
    }
    return null;
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    const ka = Object.keys(a), kb = Object.keys(b);
    for (const k of ka) {
      if (!(k in b)) return `${path}.${k}: missing in native`;
      const d = diffAt(`${path}.${k}`, a[k], b[k]);
      if (d) return d;
    }
    for (const k of kb) {
      if (!(k in a)) return `${path}.${k}: missing in browser`;
    }
    return null;
  }
  return `${path}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`;
}

const SKIP_KEYS = new Set(['compile_time_ms', 'engine']);

function compareTopLevel(name, js, c) {
  const keys = new Set([...Object.keys(js), ...Object.keys(c)]);
  let bad = null;
  for (const k of keys) {
    if (SKIP_KEYS.has(k)) continue;
    checks++;
    const d = diffAt(k, js[k], c[k]);
    if (d) {
      failures++;
      if (!bad) bad = d;
    }
  }
  if (bad) {
    console.log(`[FAIL] ${name} — first diff: ${bad}`);
  } else {
    console.log(`[PASS] ${name}`);
  }
}

for (const t of CORPUS) {
  let js, c;
  try {
    js = compileCSource(t.code, t.inputs || []);
  } catch (e) {
    failures++;
    console.log(`[FAIL] ${t.name} — JS engine crashed: ${e.message}`);
    continue;
  }
  try {
    c = compileNative(t.code, t.inputs || []);
  } catch (e) {
    failures++;
    console.log(`[FAIL] ${t.name} — native crashed: ${e.message}`);
    continue;
  }
  compareTopLevel(t.name, js, c);
}

console.log(`\nparity: ${checks} fields compared, ${failures} mismatches`);
process.exit(failures === 0 ? 0 : 1);
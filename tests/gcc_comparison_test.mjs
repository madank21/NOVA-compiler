// Differential accuracy suite: compare both NOVA engines with the system GCC
// for deterministic programs inside the documented language subset.
// This catches shared JS/C mirror bugs that the parity suite cannot detect.

import { compileCSource } from '../src/engine/compilerEngine.js';
import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const BACKEND_DIR = join(ROOT, 'c_backend');
const NATIVE = join(BACKEND_DIR, process.platform === 'win32' ? 'nova_compiler.exe' : 'nova_compiler');

if (spawnSync('gcc', ['--version'], { stdio: 'ignore' }).status !== 0) {
  console.log('gcc-comparison: SKIPPED — gcc is not available.');
  process.exit(0);
}

if (!existsSync(NATIVE)) {
  try {
    execFileSync('make', ['nova_compiler'], { cwd: BACKEND_DIR, stdio: 'pipe' });
  } catch (error) {
    console.error(`gcc-comparison: failed to build native backend: ${error.message}`);
    process.exit(2);
  }
}

const CASES = [
  {
    name: 'integer_arithmetic',
    code: 'int main() { printf("%d %d %d %d\\n", 2 + 3 * 4, (2 + 3) * 4, -7 / 2, -7 % 3); return 0; }'
  },
  {
    name: 'control_flow',
    code: 'int main() { int total = 0; for (int i = 0; i < 8; i++) { if (i == 2) continue; if (i == 6) break; total += i; } do { total++; } while (total < 14); switch (total) { case 14: total += 2; break; default: total = 0; } printf("%d\\n", total); return 0; }'
  },
  {
    name: 'array_parameter',
    code: 'int sum(int a[], int n) { int total = 0; for (int i = 0; i < n; i++) total += a[i]; return total; } int main() { int a[4] = {1, 2, 3, 4}; printf("%d\\n", sum(a, 4)); return 0; }'
  },
  {
    name: 'pointer_parameter_same_name',
    code: 'int sum(int *a, int n) { int total = 0; for (int i = 0; i < n; i++) total += a[i]; return total; } int main() { int a[3] = {1, 2, 3}; printf("%d\\n", sum(a, 3)); return 0; }'
  },
  {
    name: 'char_array_decay',
    code: 'int main() { char text[6] = "hello"; text[0] = \'H\'; text[4] = \'!\'; printf("%s %c %d\\n", text, text[1], text[4]); return 0; }'
  },
  {
    name: 'partial_array_reentry',
    code: 'int f(int set) { int a[4] = {1, 2}; if (set) a[2] = 99; return a[2]; } int main() { int first = f(1); int second = f(0); printf("%d %d\\n", first, second); return 0; }'
  },
  {
    name: 'nested_struct',
    code: 'struct I { int x; int y; }; struct O { struct I inner; int z; }; int main() { struct O o = {1, 2, 3}; printf("%d %d %d\\n", o.inner.x, o.inner.y, o.z); return 0; }'
  },
  {
    name: 'struct_array_flat_init',
    code: 'struct S { int values[3]; int tag; }; int main() { struct S s = {1, 2, 3, 4}; printf("%d %d %d %d\\n", s.values[0], s.values[1], s.values[2], s.tag); return 0; }'
  },
  {
    name: 'struct_string_init',
    code: 'struct S { char text[4]; int value; }; struct S g = {"ok", 7}; int main() { struct S s = {"hi", 3}; printf("%s %d %s %d\\n", g.text, g.value, s.text, s.value); return 0; }'
  },
  {
    name: 'struct_array_member_and_address',
    code: 'struct S { char text[4]; double nums[2]; int value; }; int main() { struct S s; s.text[0] = \'h\'; s.text[1] = \'i\'; s.text[2] = 0; s.nums[0] = 3.5; s.nums[1] = 5.5; s.value = 3; int *p = &s.value; *p = 8; printf("%s %.2f %.2f %d\\n", s.text, s.nums[0] / 2, s.nums[1] / 2, s.value); return 0; }'
  },
  {
    name: 'lexical_shadowing',
    code: 'int g = 1; int f() { int g = 2; { int g = 3; printf("%d ", g); } printf("%d ", g); return g; } int main() { int result = f(); printf("%d %d\\n", result, g); return 0; }'
  },
  {
    name: 'double_return_and_builtin',
    code: 'double value() { return 3.5; } int main() { printf("%.6f %.6f\\n", value() / 2, sqrt(2.0) / 2); return 0; }'
  },
  {
    name: 'double_array_and_pointer',
    code: 'int main() { double a[2] = {3.5, 5.5}; double *p = a; a[0] /= 2; printf("%.2f %.2f\\n", a[0], p[1] / 2); return 0; }'
  },
  {
    name: 'printf_rounding',
    code: 'int main() { printf("%.0f %.0f %.1f %.1f\\n", 2.5, 3.5, 2.25, 1.15); printf("%e %g %G\\n", 0.0001, 1e20, 1234567.0); return 0; }'
  },
  {
    name: 'scientific_literals',
    code: 'int main() { double a = 1e20; double b = 1.5e-3; printf("%f %.6f\\n", a, b); return 0; }'
  },
  {
    name: 'bitwise_and_casts',
    code: 'int main() { int x = 0x55; double d = 3.9; printf("%d %d %d %d\\n", x << 2, x ^ 0xAA, ~x, (int)d); return 0; }'
  }
];

const dir = mkdtempSync(join(tmpdir(), 'nova-gcc-'));
let failures = 0;

function nativeCompile(file) {
  try {
    return JSON.parse(execFileSync(NATIVE, [file], {
      encoding: 'utf8',
      maxBuffer: 64 * 1024 * 1024
    }));
  } catch (error) {
    if (error.stdout) {
      try { return JSON.parse(error.stdout); } catch { /* report below */ }
    }
    throw error;
  }
}

try {
  for (let i = 0; i < CASES.length; i++) {
    const test = CASES[i];
    const source = join(dir, `${i}.c`);
    const binary = join(dir, process.platform === 'win32' ? `${i}.exe` : `${i}.out`);
    writeFileSync(source, `#include <stdio.h>\n#include <math.h>\n${test.code}\n`);

    try {
      execFileSync('gcc', ['-std=gnu11', '-O0', '-Wall', '-Wextra', source, '-lm', '-o', binary], {
        encoding: 'utf8', stdio: 'pipe'
      });
      const expected = execFileSync(binary, [], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 });
      const browser = compileCSource(test.code, []);
      const native = nativeCompile(source);
      const ok = browser.success && native.success &&
        browser.consoleOutput === expected && native.consoleOutput === expected;
      if (ok) {
        console.log(`[PASS] ${test.name}`);
      } else {
        failures++;
        console.log(`[FAIL] ${test.name}`);
        console.log(`  gcc:    ${JSON.stringify(expected)}`);
        console.log(`  browser:${JSON.stringify(browser.consoleOutput)} success=${browser.success}`);
        console.log(`  native: ${JSON.stringify(native.consoleOutput)} success=${native.success}`);
      }
    } catch (error) {
      failures++;
      console.log(`[FAIL] ${test.name} — ${error.message}`);
      if (error.stderr) console.log(String(error.stderr).trim());
    }
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}

console.log(`\ngcc-comparison: ${CASES.length - failures}/${CASES.length} programs matched byte-for-byte`);
process.exit(failures === 0 ? 0 : 1);
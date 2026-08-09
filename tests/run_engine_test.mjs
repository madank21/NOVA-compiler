import { compileCSource } from '../src/engine/compilerEngine.js';

const tests = [
  { name: 'hello_world', code: `#include <stdio.h>\nint main(){ printf("Hello\n"); return 0; }`, expectedSuccess: true },
  { name: 'empty', code: ``, expectedSuccess: true },
  { name: 'nested', code: `int main(){ int x = (((1+2)*3) + (4*(5+6))); printf("%d\n", x); return 0; }`, expectedSuccess: true },
  { name: 'comments_and_strings', code: `#include <stdio.h>\n// line comment\n/* block comment */\nint main(){ printf("Line\\nSecond\\tTabbed\\\"Quote\\\"\n"); return 0; }`, expectedSuccess: true },
  { name: 'numeric_edge', code: `int main(){ int a = 2147483647; int b = -2147483648; printf("%d %d\n", a, b); return 0; }`, expectedSuccess: true },
  { name: 'malformed', code: `int main( { return 0; }`, expectedSuccess: false },
  { name: 'undefined_ident', code: `int main(){ int x = y + 1; return 0; }`, expectedSuccess: false },
  { name: 'scanf_input', code: `#include <stdio.h>\nint main(){ int n; scanf("%d", &n); printf("%d\n", n); return 0; }`, expectedSuccess: true },
  { name: 'if_statement', code: `#include <stdio.h>\nint main(){ int x = 5; if (x > 3) printf("OK\n"); else printf("NO\n"); return 0; }`, expectedSuccess: false },
  { name: 'for_loop', code: `#include <stdio.h>\nint main(){ int sum = 0; for (int i = 1; i <= 3; i++) sum = sum + i; printf("%d\n", sum); return 0; }`, expectedSuccess: false },
  { name: 'while_loop', code: `#include <stdio.h>\nint main(){ int n = 3; int fact = 1; while (n > 1) { fact = fact * n; n = n - 1; } printf("%d\n", fact); return 0; }`, expectedSuccess: false },
  { name: 'function_call', code: `#include <stdio.h>\nint add(int a, int b) { return a + b; }\nint main(){ printf("%d\n", add(2,3)); return 0; }`, expectedSuccess: false },
  { name: 'recursion_factorial', code: `#include <stdio.h>\nint fact(int n) { if (n <= 1) return 1; return n * fact(n-1); }\nint main(){ printf("%d\n", fact(5)); return 0; }`, expectedSuccess: false },
  { name: 'array_access', code: `#include <stdio.h>\nint main(){ int a[3] = {1,2,3}; int x = a[1]; printf("%d\n", x); return 0; }`, expectedSuccess: false },
  { name: 'pointer_arithmetic', code: `#include <stdio.h>\nint main(){ int x = 5; int *p = &x; *p = *p + 2; printf("%d\n", x); return 0; }`, expectedSuccess: false },
  { name: 'struct_field', code: `#include <stdio.h>\nstruct S { int x; };\nint main(){ struct S s; s.x = 7; printf("%d\n", s.x); return 0; }`, expectedSuccess: false }
];

(async function run() {
  for (const t of tests) {
    try {
      const res = compileCSource(t.code, ['42']);
      const expectedSuccess = t.expectedSuccess !== undefined ? t.expectedSuccess : true;
      const ok = res.success === expectedSuccess;
      console.log('--- TEST:', t.name, ok ? 'OK' : 'FAIL');
      console.log(JSON.stringify({ name: t.name, success: res.success, expectedSuccess, ok, compile_time_ms: res.compile_time_ms, consoleOutput: res.consoleOutput || res.console, tokens: res.tokens?.length, ast: res.ast, symbolTableCount: res.symbolTable?.length, vmTraceSteps: (res.vmTrace || []).length, diagnostics: res.diagnostics || [] }, null, 2));
      if (!ok) process.exitCode = 2;
    } catch (e) {
      console.log('--- TEST:', t.name, 'ERROR');
      console.error(e);
    }
  }
})();

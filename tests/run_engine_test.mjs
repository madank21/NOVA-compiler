import { compileCSource } from '../src/engine/compilerEngine.js';

const tests = [
  { name: 'hello_world', code: `#include <stdio.h>\nint main(){ printf("Hello\n"); return 0; }` },
  { name: 'empty', code: `` },
  { name: 'nested', code: `int main(){ int x = (((1+2)*3) + (4*(5+6))); printf("%d\n", x); return 0; }` },
  { name: 'comments_and_strings', code: `#include <stdio.h>\n// line comment\n/* block comment */\nint main(){ printf("Line\\nSecond\\tTabbed\\\"Quote\\\"\n"); return 0; }` },
  { name: 'numeric_edge', code: `int main(){ int a = 2147483647; int b = -2147483648; printf("%d %d\n", a, b); return 0; }` },
  { name: 'malformed', code: `int main( { return 0; }` },
  { name: 'undefined_ident', code: `int main(){ int x = y + 1; return 0; }` },
  { name: 'scanf_input', code: `#include <stdio.h>\nint main(){ int n; scanf("%d", &n); printf("%d\n", n); return 0; }` }
];

(async function run() {
  for (const t of tests) {
    try {
      const res = compileCSource(t.code, ['42']);
      const expectedSuccess = !(t.name === 'malformed' || t.name === 'undefined_ident');
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

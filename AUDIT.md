# NOVA Compiler Studio — Full Project Audit

Date: 2026-08-11 · Branch: `arena/019ff124-nova-compiler` (base `65cafc3`)
Scope: frontend (React 18 + Vite + Tailwind), browser JS engine (`src/engine/compilerEngine.js`), native C99 backend (`c_backend/`), tests, Docker/deployment.

> ## Resolution status (implemented in this branch)
>
> Every Critical/High finding in this audit has been fixed. Verified state:
>
> | Area | Before | After |
> |---|---|---|
> | C backend build | **did not compile** (`json.c`) | warning-free build (`-Wall -Wextra`), ASan-clean |
> | C language support | `#include` swallowed the program; comparisons compiled as addition; `for` dropped; no diagnostics | full v1 subset: control flow, arrays, pointers, structs, recursion, printf/scanf — see `docs/SCHEMA.md` |
> | JS engine | regex "compiler", `Function()` eval, hardcoded fake outputs | real lexer→parser→semantic→TAC→optimizer→bytecode→VM pipeline |
> | Parity | impossible (different schemas) | **0 mismatches across 720 field comparisons on 47 programs** (`tests/parity_test.mjs`) |
> | Engine tests | 16 cases, 2 failing, wrong expectations | 327 assertions (JS) + 79 assertions (C), all passing |
> | Fabricated UI data | 31.4% fallbacks, fake factorial frames, fake badges | all removed; real trace-driven panels |
> | Server | 4KB single `recv`, no timeouts, LAN-bound, CORS `*` | Content-Length aware reads, 256KB cap, timeouts, SO_REUSEADDR, threads, routing, 127.0.0.1 default, logging |
> | Docker | dev server in image, no C backend | multi-stage prod image + healthcheck + supervised backend |
>
> Remaining roadmap items are Phase 3/4 UX and production polish (see §10).


**Verification performed during this audit (not assumed):**

| Check | Result |
|---|---|
| `make` in `c_backend/` | **FAILS**: `json.c:119: error: 'temp' undeclared` — the entire native backend does not build |
| `gcc … -lws2_32` on Linux | **FAILS**: `ld: cannot find -lws2_32` (Makefile links it unconditionally) |
| `npm run test:engine` | **2 of 16 tests FAIL** (`comments_and_strings`, `scanf_input`), exit code 2 |
| Compile the 6 bundled presets with the JS engine | **5 of 6 return `success=false`**; console shows garbage (`NaN`, wrong values) |
| `make test_native` | Would fail even after fixing the compile error (duplicate `main` in link, assertion unreachable) |
| `dist/` | Build artifacts are **committed to git** |

---

## 1. Executive summary

NOVA Compiler Studio has a credible *skeleton*: real phase modules in C (lexer → parser → semantic → TAC → optimizer → bytecode → VM → JSON), a React UI with one panel per phase, and a JS "mirror" engine. However, the audit shows the project is **not functionally complete and is currently non-deterministic and partly fake**:

1. **The native C backend does not compile** (`json.c` bug), and even if it did, its Makefile targets are broken on Linux (`-lws2_32`, duplicate `main`). The "native API" the UI advertises cannot run as shipped.
2. **The C compiler silently miscompiles or drops most of the language**: the preprocessor-skip loop in `parse_program` eats the program after `#include`, relational operators are codegen'd as **addition**, `for` statements are dropped from TAC entirely, there is no unary minus, no `++`, no arrays, no structs, no error diagnostics of any kind.
3. **The JS engine is largely simulated**: AST/TAC/symbol table are built with line- and regex-heuristics (not from a parse), the "VM" interprets raw source text with `new Function()` eval, optimization metrics are fabricated constants, and console output falls back to hardcoded strings like `'5! = 120'`. Diagnostics are regex scans of raw text that fire **inside comments and string literals**.
4. **The UI presents fabricated data as compiler output in several places** (`factorial(n=5)` call frames, `31.4%` reduction fallbacks, an unconditional "Scope & Type Checks Passed" badge, fake compile-time fallbacks).
5. **JS and C outputs use different schemas** (token type names, TAC opcode vocabularies, missing `diagnostics`/`waitingForInput` in C), so parity is impossible until a shared contract is defined.
6. **Testing is one JS script (currently red) and one C smoke test that cannot link.** There are no unit tests per phase, no malformed-input tests, no JS↔C parity tests, no CI.
7. **Deployment runs the Vite dev server in Docker**, omits the C backend entirely, has no health check, no prod build, and the frontend hardcodes `http://localhost:8080`.

Verdict: the architecture (phase-per-module + JSON contract + visualizers) is sound and worth keeping, but the compiler cores on **both** sides need a recode around a shared language subset and a shared JSON schema before any other investment makes sense.

---

## 2. Critical missing or inaccurate functionality

### C-1. Native backend does not compile — `json.c` — **Critical**
- File/function: `c_backend/json.c`, `serialize_symbol_table()` line 119.
- Evidence: `error: 'temp' undeclared (first use in this function)` — verified with `gcc -Wall -Wextra -O2`.
- Why it matters: `nova_compiler`, `nova_server`, and `make test_native` all fail; the "Pure C Server (8080)" mode in the UI is unreachable.
- Fix: declare `char temp[64];` at the top of `serialize_symbol_table()` (mirroring other serializers).
- Type: trivial recode. **Priority: P0 (day 1).**

### C-2. Parser eats the program after `#include` — **Critical**
- File/function: `c_backend/parser.c`, `parse_program()`:
  ```c
  if (tok.type == TOKEN_INCLUDE || tok.type == TOKEN_DEFINE) {
      advance_token(parser);
      while (peek != TOKEN_SEMICOLON && peek != TOKEN_EOF) advance_token(parser); // ← eats real code
      match_token(parser, TOKEN_SEMICOLON);
  ```
- `#include <stdio.h>` has no semicolon, so this loop consumes tokens up to the **first semicolon of the actual program** — for every preset that is `int main() { int x = 5;`. Verified by tracing: `main` never becomes a `NODE_FUNCTION_DEF`; `int y = 10` is parsed as a *global* declaration; `printf` is token-skipped and never becomes a call. Consequently `native_test.c`'s core assertion (`console contains "15"`) can never pass even once the build is fixed.
- Fix: the lexer should emit `#include` as a single directive token plus a `<header>`/`"header"` argument token (track line endings so the parser skips exactly one logical line), or the parser skips until `token.line` changes.
- Type: recode (lexer + parser). **Priority: P0.**

### C-3. Relational/logical operators miscompile to addition — **Critical**
- File/function: `c_backend/tac.c`, `generate_expr_tac()`, `NODE_BINARY_OP` branch.
  ```c
  TACOpcode op = TAC_ADD;
  if (strcmp(node->op, "+") == 0) op = TAC_ADD;
  else if (strcmp(node->op, "-") == 0) ... // no cases for <, >, <=, >=, ==, !=, &&, ||
  ```
  Any comparison (`x < y`, `n <= 1`) silently falls through to `TAC_ADD`.
- Why it matters: every `if`/`while` condition in the language subset computes wrong values; this is silent miscompilation, the worst class of compiler bug.
- Fix: add `TAC_LT/TAC_GT/TAC_LEQ/TAC_GEQ/TAC_EQ/TAC_NEQ` (and logical) opcodes end-to-end (tac.h enum, emitter, optimizer pass-through, bytecode `OP_CMP_*`, VM).
- Type: recode. **Priority: P0.**

### C-4. `for` statements dropped from TAC — **Critical**
- File/function: `c_backend/tac.c`, `generate_stmt_tac()` — there is **no `NODE_FOR_STMT` case**; for-loops vanish silently (and the parser stores `for` init/increment in AST fields nothing consumes). Also `NODE_EXPRESSION_STMT` containing calls works, but increment expressions like `i++` are unparseable anyway (see C-6).
- Fix: desugar `for(init; cond; incr) body` to `init; L_start: ifFalse(cond) L_end; body; incr; goto L_start; L_end:` in TAC.
- Type: recode. **Priority: P0.**

### C-5. Bytecode buffer overflow — **Critical (memory safety)**
- File/function: `c_backend/bytecode.c`, `generate_bytecode()`. `chunk->capacity = 64` is allocated once and every emission does `chunk->code[chunk->count++]` with **no bounds check and no realloc** anywhere. One TAC binary op emits 4 instructions, so ~16 TAC instructions overflow the heap buffer.
- Fix: add `static void chunk_ensure(BytecodeChunk*, int extra)` with `realloc`, call before every write; check `realloc`/`malloc` returns (currently unchecked project-wide).
- Type: recode. **Priority: P0.**

### C-6. Language constructs the parsers cannot handle (both engines) — **Critical for the advertised feature set**
- C parser (`parser.c`): no unary `-`/`!`/`*`/`&` (parse_primary's fallback `advance; return NODE_INT_LITERAL(0)` **silently fabricates a 0 literal**), no `++/--`, no compound assignment (`+=` tokens exist but are unhandled), no arrays/initializer lists, no struct member access, no `do`/`switch`/`break`/`continue`, no `char` literals (`'a'` → `TOKEN_ERROR` in `lexer.c` — the `TOKEN_CHAR_LITERAL` enum is dead code), no diagnostics or error recovery at all (all `match_token` results ignored).
- JS engine: everything control-flow related is *deliberately rejected* by `unsupportedPatterns` regexes in `compileCSource()` (`if`, `for`, `while`, `struct`, arrays, pointers, non-main functions), and `tests/run_engine_test.mjs` **codifies those failures as expected** (`expectedSuccess: false` for if/for/while/functions/arrays/pointers/structs).
- Evidence: 5 of the 6 presets bundled in `src/engine/presets.js` (factorial, fibonacci, bubble_sort, array_sum, struct_demo) fail to compile in the browser engine — verified by running the engine on each preset:
  ```
  factorial   success=false  console="Factorial of 5 = 0\n"
  fibonacci   success=false  console="NaN \n"
  bubble_sort success=false  console="Sorted Array: NaN \n"
  array_sum   success=false  console="Total Sum = 0\n"
  struct_demo success=false  console="Student ID: 4, GPA: 101.00\n"  (correct: 101, 3.92)
  ```
- Fix: define the v1 language subset (recommend: int/float scalars, arrays, `if/else`, `while`, `for`, functions with recursion, printf/scanf; defer structs/pointers) and implement it **in both engines**, or implement it once in C and compile the C backend to WASM for the browser (recommended — see §4/R-1).
- Type: recode. **Priority: P0.**

### C-7. The JS "VM" is a source-text interpreter with hardcoded outputs — **Critical**
- File/function: `src/engine/compilerEngine.js`, `runInteractiveVirtualMachine()`.
  - It never executes the generated bytecode; it regex-parses source lines.
  - Expressions are evaluated with `Function('"use strict"; return (' + evalExpr + ')')()` — arbitrary evaluation of user text, and C expressions that aren't valid JS silently produce `NaN`/0.
  - `printf` formatting consumes specifiers in the wrong order: for `printf("Student ID: %d, GPA: %.2f\n", s1.id, s1.gpa)` the first arg replaces `%.2f` (because that branch is checked first), producing `Student ID: 4, GPA: 101.00`.
  - If nothing was printed, it returns **hardcoded** strings keyed on substrings: `'factorial' → '5! = 120\n'`, `'fibonacci' → '0 1 1 2 3 5 8 13\n'`, `'bubbleSort' → …`, else `'Program executed successfully.'`.
  - `scanf` echoes the input back into output (real scanf doesn't).
- Why it matters: the "Phase 7 — VM Execution" panel and the console show fiction; this is exactly what the audit brief asks to detect.
- Fix: delete the line interpreter; execute the generated bytecode on a real stack VM shared (schema-wise) with the C VM: variables map, operand stack, `PUSH/LOAD/STORE/ADD…/PRINT/INPUT/HALT`, step trace recording, input-queue suspension.
- Type: recode. **Priority: P0.**

### C-8. JS diagnostics are raw-text regexes that fire inside comments/strings — **Critical**
- File/function: `compilerEngine.js`, `checkBrackets()` + `unsupportedPatterns` + the undefined-identifier scan.
- Evidence from `npm test` (currently red):
  - `comments_and_strings` fails because `/* block comment */` matches the "pointer" regex `\*\s*[a-zA-Z_]` (`*` then space then `block`) → `Unsupported pointer/dereference syntax`.
  - `scanf_input` fails because `&n` matches the address-of regex — **every scanf program is rejected** although the VM pretends to support scanf.
  - Running preset `array_sum` yields diagnostic `Undefined identifier 'd' in expression for 'Sum'` — the assignment regex matched **inside the printf format string** (`"Total Sum = %d\n", sum`).
- Fix: diagnostics must come from the lexer/parser/semantic phases over tokens/AST with line/column, never over raw text.
- Type: recode. **Priority: P0.**

### C-9. Build tooling for the C backend is broken — **High**
- File: `c_backend/Makefile`.
  - `test_native: $(OBJS) native_test.o` — `OBJS` includes `main.o`, and `native_test.c` has its own `main` → duplicate-symbol link error (verified).
  - `$(SERVER_TARGET)` links `-lws2_32` unconditionally → fails on Linux (verified: `ld: cannot find -lws2_32`); only valid for MinGW.
  - `package.json` `"test:c-backend": "cd c_backend && make test_native"` therefore can never pass; `npm test` doesn't include it anyway.
- Fix: `OBJS := lexer.o parser.o semantic.o tac.o optimizer.o bytecode.o vm.o` (no `main.o`); link `ws2_32` behind `ifeq ($(OS),Windows_NT)`; add a `check` target; wire both suites into `npm test`.
- Type: small recode. **Priority: P0.**

### C-10. Fabricated metrics and timings — **High**
- JS `optimizeTACList()` returns invented numbers: `constant_fold: max(1, floor(len*0.2))`, `reduction_percentage: 24.5` fixed; `compile_time_ms` is clamped to `Math.max(1.2, …)`.
- C `optimizer.c` implements only 2 of the 4 advertised passes (no constant propagation, no DCE), folds through `double`/`"%.0f"` (breaks float semantics and integer division), and computes `reduction_percentage` as *transformations/instructions* even though the instruction count never changes.
- UI amplifies this with fallbacks (§5/F-3).
- Fix: implement passes that actually rewrite TAC (fold, propagate constants via copy chains, mark dead stores, strength-reduce), count real before/after instruction counts, drop the JS clamp.
- Type: recode. **Priority: P1.**

---

## 3. Components / features to add

| # | Component | Why | Type | Priority |
|---|---|---|---|---|
| A-1 | **Shared JSON schema contract** (`docs/SCHEMA.md` + generated fixtures) | JS and C currently emit different shapes (§7); parity tests are impossible without it | docs + tests | P0 |
| A-2 | **Diagnostics array in C output** (`{level, msg, line, column}`), `success=false` path in `json.c`, HTTP 200-with-diagnostics (or 422) | C currently has no diagnostics and always `"success":true`; `SymbolTablePanel` shows a hardcoded pass badge | recode | P0 |
| A-3 | **`GET /api/health` + `/api/version`** on `nova_server` | Nothing exists for Docker HEALTHCHECK or UI engine detection | new code | P1 |
| A-4 | **Engine indicator in UI** (badge: "Pure C server" vs "Browser engine (fallback)") | Fallback happens silently via `console.warn` only; users can't tell which engine produced results | new component | P1 |
| A-5 | **Real stdin model in both engines**: `inputs: string[]` in request/response + `waitingForInput`/`inputPrompt` in C JSON | Native mode ignores `userInputs`; C JSON has no input fields at all | recode + schema | P1 |
| A-6 | **Vite dev proxy** (`/api` → `http://localhost:8080`) | Removes hardcoded origin + CORS need (§9/D-3) | config | P1 |
| A-7 | **Parity test harness**: golden `.c` programs with expected tokens/AST/TAC/bytecode/console, run against JS engine and `nova_compiler --json` | Core of a deterministic-compiler claim | new tests | P1 |
| A-8 | **Optimizer pass toggles + per-pass diff** in TACPanel | High learning value; passes become observable | new feature | P3 |
| A-9 | **Breakpoints & step-to-cursor in VMVisualizer**; source-line ↔ bytecode-pc mapping gutter | The trace already carries `line`; this is cheap and high-value | new feature | P3 |
| A-10 | **Save programs to localStorage + shareable URL (base64/deflate in hash)** | Currently presets are the only persistence | new feature | P3 |
| A-11 | **C-to-WASM build** (Emscripten target in CMake) | One compiler implementation, deterministic in-browser, kills an entire class of parity bugs | new component | P2 (see R-1) |
| A-12 | **Language-subset reference panel** (grammar EBNF, opcode table, TAC op reference) | Learners need to know what's supported; today "unsupported" is discovered by failing | docs/new component | P2 |

---

## 4. Code that should be rewritten or refactored

### R-1. `src/engine/compilerEngine.js` — full recode (or better: replace with WASM of the C backend) — **P0**
Every phase is a heuristic: `buildDynamicAST`/`generateDynamicTAC` scan **source lines with regexes** (a declaration inside a brace-dedented line, or two statements on one line, breaks them); `buildDynamicSymbolTable` pattern-matches token triples (`type ident (`), misses pointer declarations (`int *p`), never adds parameters, fabricates addresses (`0x1000+4i`) and `params` counts; `generateDynamicBytecode` emits `PUSH parseInt(rhs,10) || 0` — **every non-numeric assignment compiles to `PUSH 0`**; there are no arithmetic opcodes in the JS bytecode at all. Recommended path: fix the C backend first, add an Emscripten target, load it in the browser as the "browser engine" (same binary, guaranteed parity). Keep the JS engine only if a pure-JS implementation is a hard requirement — in that case rewrite it as a real recursive-descent compiler mirroring the C modules 1:1.

### R-2. `c_backend/parser.c` — recode with diagnostics and recovery — **P0**
- Add a diagnostic sink (`parser_error(parser, line, col, msg)`), panic-mode recovery (skip to `;`/`}` on error), and stop returning fake `NODE_INT_LITERAL(0)` for unexpected tokens.
- Implement the missing grammar (§2/C-6): unary ops, `++/--`, `+=`, arrays (`int a[5] = {…}`), member access later.
- Fix the `#include` handling (C-2).
- Check every `match_token` result and report mismatches ("expected ')'").

### R-3. `c_backend/tac.c` — recode codegen — **P0**
- Map all binary ops including comparisons/logical (C-3); emit `TAC_FOR`-desugared loops (C-4); handle `NODE_EXPRESSION_STMT` calls for `scanf` (`TAC_READ`); stop leaking `strdup(new_temp(...))` (every expression leaks ~64 bytes) — temps/labels should be arena-allocated or caller-owned buffers; the `static char buf[64]` in `new_temp/new_label` is also not thread-safe.

### R-4. `c_backend/vm.c` — recode execution core — **P1**
- Implement `OP_CALL/OP_RET/OP_JMP/OP_JZ` (currently `default: break` — functions and branches don't execute), `OP_MOD`, array/pointer ops (`OP_LOAD_ARRAY`… exist in the enum but are never emitted or executed — dead code).
- `OP_PRINT` must consume a format string + args from the program's printf call (currently prints `"Output: %d\n"` regardless of the program — `native_test.c`'s expectation of `"15"` depends on luck).
- Division by zero currently yields silent `0`; emit a runtime diagnostic and halt.
- Bound execution (max steps) against infinite loops.

### R-5. `c_backend/semantic.c` — refactor — **P1**
- Produce real diagnostics (`error_count++` with no message/line is useless).
- Function scopes are created but **never linked into `scope_list`** (`scope->next` untouched) → `serialize_symbol_table` walks only the global scope, so **no local variable ever appears in the JSON symbol table**, and `symbol_table_free` leaks every function scope (verified by reading `create_scope`/`traverse_ast_semantic`).
- `total_symbols` is never incremented (dead field). Add type checking of assignments/args (currently zero type checking despite the UI badge "Scope & Type Checks Passed").

### R-6. `c_backend/server.c` — recode HTTP layer — **P1** (details §6).

### R-7. `c_backend/json.c` — refactor serializer — **P1**
- Add `success`, `diagnostics`, `waitingForInput`/`inputPrompt`; don't hardcode `"instruction":"EXEC_LINE %d"` in `serialize_vm_trace` (decode the real opcode); escape AST `identifier/type_name/op` fields through `buf_append_escaped`; include `string_val` (printf format strings are currently dropped from the AST JSON); use `%.17g` for `float_val` (currently `%.2f`, lossy).

### R-8. `src/components/VMVisualizer.jsx` — remove fabricated call stack — **P1**
```js
if (currentStep.instruction.includes('CALL') || currentStepIdx > 3) {
  callStackFrames.push({ frame: 1, func: 'factorial(n=5)', ... }); // ← hardcoded fiction
}
```
Render frames from actual `CALL`/`RET` trace events (requires R-4) or show "No active frames".

### R-9. `src/components/Editor.jsx` — refactor or replace — **P2**
The transparent-textarea-over-highlight-overlay is fragile: highlight lines get `px-1` padding the textarea text doesn't have (horizontal desync), Tab is not handled, and the tokenizer hardcodes preset names (`'factorial','fibonacci','swap','bubbleSort'`) as "functions". Recommend CodeMirror 6 (`@codemirror/lang-c`), which also brings real a11y, gutters for breakpoints/diagnostics, and correct scrolling.

### R-10. Delete committed build artifacts — **P2**
`dist/` is tracked in git (`git ls-files` shows `dist/index.html`, `dist/assets/*`). Remove from the index, keep `.gitignore` (already ignores `dist/` — the files were force-added), rebuild in CI/Docker.

---

## 5. Frontend improvements

| # | Severity | File / location | Finding | Fix | Type | Priority |
|---|---|---|---|---|---|---|
| F-1 | High | `src/App.jsx` `handleCompile` | Hardcoded `http://localhost:8080/api/compile`; breaks on any non-local host and becomes mixed-content over HTTPS; fallback is silent; no `AbortController` → rapid recompiles race (last-arrived response wins, not last-sent); native mode ignores `userInputs` | Vite proxy `/api` (A-6), engine badge (A-4), request-id/Abort guard, send inputs | recode | P1 |
| F-2 | High | `tests/run_engine_test.mjs` | The suite is **red** (2 fails) and half its cases assert that valid C *should fail* — it codifies broken behavior | Rewrite expectations once engine supports the subset; run in CI | tests | P1 |
| F-3 | High | `App.jsx` tabs (`\|\| 31.4`), `Header`/`ConsoleOutput` (`\|\| 3.2`), `TACPanel.jsx` (`\|\| 31.4`, `\|\| 2`, `\|\| 1`), `SymbolTablePanel.jsx` ("Scope & Type Checks Passed" unconditional), `VMVisualizer.jsx` (factorial frames) | UI shows **fabricated fallback values** when data is missing — indistinguishable from real compiler output | Show `—`/empty states; compute badges from real data; remove every fake constant | recode | P1 |
| F-4 | Medium | `App.jsx` `handleTerminalInput` | Each scanf submit **recompiles the whole program from scratch** replaying all inputs — O(n²) and re-runs side-effecting phases; also `useEffect(handleCompile, [])` double-runs under StrictMode | Keep a compiled program object; feed inputs to the VM without recompiling (VM must support input queue) | recode | P2 |
| F-5 | Medium | `index.html` (`<body class="select-none">`, favicon `/vite.svg` missing) | Text selection disabled app-wide → users **cannot copy console output** (the console even styles `selection:` — dead styling); favicon 404s | `select-text` on console/panels; add real favicon | small fix | P2 |
| F-6 | Medium | A11y across components | Icon-only buttons without `aria-label` (VMVisualizer play/pause/reset), speed `<input type="range">` and preset `<select>` unlabeled, tab strip is buttons without `role="tablist"/aria-selected` or arrow-key navigation, compile status not announced (`aria-live`) | Add labels/roles/keyboard support; `aria-live="polite"` status region | recode | P2 |
| F-7 | Medium | `App.jsx` layout | Fixed `w-1/2` split + `grid-cols-4` VM grid + px-fixed console → unusable below ~1024 px; no responsive breakpoints at all | Tailwind `lg:`/`md:` stacking, collapsible panels | recode | P3 |
| F-8 | Medium | `Editor.jsx` | Whole-document re-highlight on every keystroke (O(lines)); overlay/textarea scroll sync via manual refs is jittery | Replace with CodeMirror (R-9) or memoize line rendering | recode | P2 |
| F-9 | Low | `TokensPanel.jsx`, `BytecodePanel.jsx` | No virtualization — large sources render thousands of rows; token badge colors keyed on JS-only type names (native tokens all render gray, see §7) | react-window or pagination; engine-agnostic color map | recode | P3 |
| F-10 | Low | `package.json` | `"lint": "eslint ."` but **no eslint dependency or config** — script is dead | Add eslint + `eslint-plugin-react-hooks` config (would have caught the effect deps issues), or remove script | config | P2 |
| F-11 | Low | `ConsoleOutput.jsx` | `isSuccess` derived as `compileResult?.success !== false` → a *missing* result shows green "Execution Finished"; `CheckCircle2` icon used for the failure state | Explicit tri-state (idle/running/error) | recode | P2 |
| F-12 | Low | `vite.config.js` | `server.open: true` in a Docker/CI context tries to open a browser; no `server.host`/`allowedHosts` policy for the preview proxy | `open: false`, document host config | config | P3 |

---

## 6. Native C backend improvements

| # | Severity | File / function | Finding | Fix | Type | Priority |
|---|---|---|---|---|---|---|
| B-1 | Critical | `json.c:serialize_symbol_table` | Does not compile (`temp` undeclared) | Declare buffer; add `-Werror` to CI | fix | P0 |
| B-2 | Critical | `bytecode.c:generate_bytecode` | Heap overflow: no capacity check/realloc (C-5) | `ensure_capacity` + checked realloc | recode | P0 |
| B-3 | Critical | `parser.c` (whole) | Silent acceptance of garbage, fabricated 0-literals, no diagnostics (C-2, C-6, R-2) | Diagnostic sink + panic recovery | recode | P0 |
| B-4 | High | `server.c:start_server/handle_request` | One 4 KB `recv()` per connection: no `Content-Length` parsing, no continuation reads (requests >4 KB or split across packets are truncated → the fallback literally compiles `"int main() { return 0; }"` when `\r\n\r\n` isn't in the first packet); no recv/send timeouts → a slow client pins the only thread forever; single-threaded accept loop → one request at a time; no `SO_REUSEADDR` (restart fails to bind); `strstr(buffer, "OPTIONS")` scans the **body** too; return values of `recv/send` ignored (partial sends); `parse_source_code` JSON extraction is hand-rolled `strstr` and mishandles `\\uXXXX` escapes; unbounded work per request | Proper request reader (loop until Content-Length satisfied, cap body e.g. 256 KB → 413), per-connection thread or poll loop, `SO_RCVTIMEO/SO_SNDTIMEO`, `SO_REUSEADDR`, real method/path routing (`POST /api/compile`, `GET /api/health`, 404 otherwise), or adopt a tiny vetted HTTP lib | recode | P1 |
| B-5 | High | `server.c` | Binds `INADDR_ANY` with `Access-Control-Allow-Origin: *` and no auth/size limits — reachable from the whole LAN; a malformed-input crash kills the only server process | Bind 127.0.0.1 by default (`--host` flag to widen), keep CORS only for the dev origin, add watchdog/restart docs; run ASan builds in CI | recode/config | P1 |
| B-6 | High | `semantic.c` | Scopes not linked into `scope_list` → locals invisible in JSON + memory leak (R-5); no type checking; `error_count` without messages | Link/free scopes properly; emit diagnostics | recode | P1 |
| B-7 | High | `tac.c` | Comparison ops → `TAC_ADD` (C-3); `for` dropped (C-4); `strdup` leaks per expression; `static` temp/label buffers not thread-safe | R-3 + arena allocation | recode | P0/P1 |
| B-8 | Medium | `vm.c` | `OP_CALL/RET/JMP/JZ` unimplemented; `OP_DIV` div-by-zero → silent 0; `OP_PRINT` ignores format; trace copies full 256-int stack + 2 KB console **per step** → multi-MB JSON for long runs; no step limit | R-4; cap trace (e.g. 5 000 steps + `truncated:true`); record only used stack depth (already `stack_top`) but shrink `VMStep` to dynamic arrays | recode | P1 |
| B-9 | Medium | `lexer.c` | No char literals (`'a'` → `TOKEN_ERROR`, `TOKEN_CHAR_LITERAL` dead); no hex/octal/exponent literals; unterminated strings silently accepted; `token_type_to_string` collapses ~60 token types into `"TOKEN_OPERATOR_OR_KEYWORD"` (lossy JSON); string lexeme truncated to 255 with compiler warning (`-Wformat-truncation`) | Implement char literals, `0x`/exponent, emit lexeme from length-limited copy; full type→string table shared with JS names (§7) | recode | P1 |
| B-10 | Medium | `optimizer.c` | Only fold+strength passes; folding via `double`/`%.0f` corrupts integer division (`7/2 → 4.00 → "4"` ok, but `5/2*2` chains and floats break); no const-prop/DCE yet metrics struct advertises them; `reduction_percentage` semantics wrong (C-10) | Implement passes or rename metrics to what's real; fold with typed (int/float) evaluation | recode | P1 |
| B-11 | Medium | All `malloc/realloc` sites | No NULL checks anywhere (lexer list growth, AST children, json buffer) → crash-on-OOM instead of error | Checked alloc helpers | recode | P2 |
| B-12 | Medium | `parser.c:parse_primary` | `free(node->children); free(node);` when converting identifier→call node — fragile manual free; also `ast_free` frees `left/right/condition/...` **and** `children` but a node can hold both (e.g. `NODE_FUNC_CALL` children + nothing else, ok; but `if` stores condition+left while compound stores children) — invariant is implicit and brittle | Single child-array AST representation, or explicit ownership comments + ASan CI | refactor | P2 |
| B-13 | Low | `main.c` | Only compiles a hardcoded sample; no CLI (`nova_compiler file.c --json`) | Add arg parsing + stdin/file input; needed for parity harness (A-7) | new code | P1 |
| B-14 | Low | `Makefile`/`CMakeLists.txt` | No `-g`/ASan dev target, no `install`, no sanitizer options, CMake and Make diverge | Add `make asan` (`-fsanitize=address,undefined`), keep both in sync, add `ctest` target | config | P2 |
| B-15 | Low | `vm.c:strncpy` patterns | `strncpy(dst, src, N)` without explicit NUL termination throughout (compiler warns about truncation) | `snprintf`/explicit termination helpers | refactor | P2 |

---

## 7. JS/C compiler parity issues (phase-by-phase)

**Lexing**

| Aspect | JS (`tokenizeC`) | C (`lexer.c`) | Impact |
|---|---|---|---|
| Token type names | Generic buckets: `TOKEN_KEYWORD`, `TOKEN_TYPE`, `TOKEN_OPERATOR`, `TOKEN_SEPARATOR`, `TOKEN_PREPROCESSOR` | Specific: `TOKEN_INT`, `TOKEN_PLUS`, `TOKEN_LPAREN`, `TOKEN_INCLUDE`, …; ~60 types collapse to `"TOKEN_OPERATOR_OR_KEYWORD"` in JSON | `TokensPanel` badge colors key on JS names; native tokens render all gray; no equality test possible |
| Char literals | `'a'` → three `TOKEN_SEPARATOR`s | `'a'` → `TOKEN_ERROR` | Both wrong, differently wrong |
| `#include` | one `TOKEN_PREPROCESSOR` (`#include`) then `<`, `stdio`, `.`, `h`, `>` as separators | `TOKEN_INCLUDE` then `LT/IDENT/DOT/IDENT/GT` | Parser depends on this in C (C-2) |
| String lexeme | full value kept | truncated to 255 chars (`snprintf` warning) | Divergent token tables |
| Numbers | `parseInt`/`parseFloat`, no hex | `atoll`/`atof`, no hex | Same gap both sides |

**Parsing / AST**

| JS | C | Impact |
|---|---|---|
| Built from **source lines** via regex; only `main`/decls/printf/scanf/return recognized; flat `children` only; fabricates a fake `main` node when nothing matches | Real recursive descent but: no unary/++/arrays/structs, comparisons miscompiled downstream, `for` unsupported in TAC, silent 0-literal fabrication | Two different grammars; neither handles the bundled presets; AST JSON shapes differ (C flattens `left/right/condition/else/init/increment` into `children`; JS has no `int_val/float_val/op` fields) |

**Semantic / symbol table**

| JS | C | Impact |
|---|---|---|
| Token-triple pattern match; fake addresses `0x1000+4i`; `params` hardcoded (printf/scanf `params:2`, functions `params:1`); scopes = function name string, never popped | Real scoped table but only **global scope serializes** (B-6), addresses always `0x0000`, `param_count` real but locals invisible; built-ins `params:1` | Same panel shows structurally different data per engine; "Scope & Type Checks Passed" badge is a lie in both (no type checking exists anywhere) |

**TAC**

| JS | C | Impact |
|---|---|---|
| Ops: `FUNC_BEGIN/PRINT/SCANF/ASSIGN/RETURN`; `ASSIGN` keeps the **entire RHS as one string** (not three-address); no control flow; no `FUNC_END` | Ops: `+ - * / % = LABEL GOTO IF_FALSE PARAM CALL RETURN FUNC_BEGIN FUNC_END PRINT`; real three-address temps `t0…`; but comparisons → `+` (C-3), `for` dropped (C-4), no `SCANF`/read op | Disjoint opcode vocabularies; `TACPanel` renders every row as `res = a1 a2`, which is wrong for C's `LABEL`/`GOTO`/`PARAM` rows and for JS's `PRINT` |

**Optimizer**

| JS | C |
|---|---|
| One rewrite (`MUL x 2 → ADD x x`) plus **fabricated metrics** (`reduction_percentage: 24.5` constant) | Fold + strength only; `constant_prop/dead_code` always 0 but displayed; percentage = transformations/total (misleading) |

**Bytecode**

| JS | C |
|---|---|
| `PUSH/STORE/PRINT/LOAD_PTR/RET/HALT`; `PUSH parseInt(rhs)||0` → all expressions are 0; **no LOAD, no arithmetic opcodes** | `PUSH/LOAD/STORE/ADD/SUB/MUL/DIV/PRINT/RET/HALT`; overflow risk (B-2); no `INPUT` opcode; jump/call opcodes defined but never generated |

**VM / output**

| Field | JS | C |
|---|---|---|
| Execution model | Regex line interpreter + `Function()` eval + hardcoded fallback outputs | Real bytecode interpreter, but no calls/jumps, `"Output: %d"` printing |
| `vmTrace[].instruction` | `"EXEC_LINE n"` / `"SCANF &x"` | Hardcoded `"EXEC_LINE %d"` in `json.c` |
| `vmTrace[].stack` | Always `[]` | Real values |
| `diagnostics` | Regex-derived, fire in comments/strings | **Absent** |
| `success` | from diagnostics | **Always `true`** |
| `waitingForInput`/`inputPrompt` | Present | **Absent** (native mode can never prompt for scanf input) |
| `consoleOutput` | Line interpreter or hardcoded strings | `"Output: N\n"` regardless of printf format |
| `compile_time_ms` | Fake floor `Math.max(1.2, …)` | Real `clock()` |

**Resolution:** define one schema (A-1) with exact field names, token-type enum, opcode enum and trace shape; make both engines emit it; add a parity suite (A-7) that fails CI on any divergence. Strongly consider R-1/A-11 (WASM) so parity is structural instead of tested.

---

## 8. Testing gaps with proposed test cases

Current state: `tests/run_engine_test.mjs` (16 cases, 2 failing, several asserting broken behavior) + `c_backend/native_test.c` (1 program, can't link, assertion unreachable). No unit/integration/e2e/parity/fuzz tests. No CI.

**Proposed suites**

1. **JS unit tests** (Vitest, import `compilerEngine.js` directly):
   - Lexer: `"int x = 0x1F;"` → expect hex literal or explicit diagnostic; unterminated `"abc` → error token with line; `/* /* nested */` ends at first closer; `'a'`, `'\n'` char literals.
   - Parser: precedence `2+3*4 == 14`; right-assoc assignment `a = b = 5`; dangling else binds to inner if; `int a[3] = {1,2,3};` parses; missing `;` → diagnostic with correct line/column, no infinite loop.
   - Semantic: use-before-decl, redeclaration in same scope, call with wrong arity, assignment type mismatch (int↔float warning).
   - Optimizer semantics: fold `5*2 → 10`; `x*2 → x+x` preserves result; DCE removes `t = 1` when `t` dead; **no pass may change observable output** (property test: VM output before == after).
   - VM: factorial(5)=120, fibonacci(8), div-by-zero diagnostic, scanf suspension/resume with `['42']`, infinite `while(1)` hits step cap.
2. **C unit tests** (same cases, compiled through `nova_compiler --json` after B-13) — mirror 1:1.
3. **Parity suite**: golden programs (all 6 presets + the 16 current test cases + 20 new) → run JS engine and C binary → `jq`-normalize → deep-diff tokens/AST/TAC/bytecode/console; must be byte-identical on `consoleOutput` and `success`.
4. **Malformed-input corpus** (both engines, must never crash and must report ≥1 diagnostic): empty file; only `}`; `int = 5;`; `int main( {`; `printf("unterminated`; `int a[;` ; 10 000-deep `((((1))))`; 1 MB identifier; UTF-8 garbage; `#include` with no newline at EOF; CRLF source.
5. **Server tests**: `POST /api/compile` with text/plain and JSON bodies; >4 KB body; body with embedded `"OPTIONS"`; slowloris (timeout enforced); concurrent 10 requests; malformed JSON `{source: unquoted`; `/api/health` returns 200.
6. **Frontend e2e** (Playwright): load app → tokens table non-empty for hello_world; compile preset 02 → console shows `Factorial of 5 = 120`; enter scanf input → program resumes; native-mode fallback shows engine badge; tab keyboard navigation.
7. **CI** (GitHub Actions): `npm ci && npm run lint && npm test`, `make -C c_backend asan check`, parity suite, `vite build`, Docker build. Today none of this exists.

---

## 9. Docker, deployment, and security improvements

| # | Severity | Finding (file) | Fix | Type | Priority |
|---|---|---|---|---|---|
| D-1 | High | `Dockerfile` ships `npm run dev` as the runtime: dev server in "production", no prod build, **C backend never built/packaged**, no HEALTHCHECK, `EXPOSE 3000` only | Multi-stage: (1) `node:22-alpine` → `npm ci && npm run build`; (2) `alpine` + `gcc make` → build `nova_server`; final stage: nginx (or `vite preview`) serving `dist/` + `nova_server` behind it; `HEALTHCHECK CMD wget -qO- localhost/api/health`; supervisor/entrypoint script for both processes | recode | P1 |
| D-2 | High | Browser calls `http://localhost:8080` directly (App.jsx) → impossible from a container/preview host, mixed-content over HTTPS, CORS needed | Frontend calls relative `/api/compile`; nginx/vite proxy → `nova_server` on 127.0.0.1; server drops `*` CORS | recode + config | P1 |
| D-3 | Medium | No environment configuration anywhere: port 8080 hardcoded (`server.c #define PORT`), no `PORT` env, no `.env` in Vite | `NOVA_PORT` env with default; `import.meta.env.VITE_API_URL` fallback chain | recode | P2 |
| D-4 | Medium | No logging: server prints one startup line; requests, errors, timings unlogged | Structured line per request (`method path bytes ms status`), `-v` flag | new code | P2 |
| D-5 | Medium | Committed `dist/` artifacts + `.gitignore` contains `nova.tar` (an image tarball existed) → repo bloat, stale artifacts served | `git rm -r --cached dist`, R-10; never commit image tars | hygiene | P2 |
| D-6 | Medium | Security posture: `INADDR_ANY` + CORS `*` + no auth + no body cap + `Function()` eval in browser engine + unbounded VM | B-4/B-5 fixes; replace eval (C-7); step caps (B-8); note in README that the server is a teaching tool not a public service | recode + docs | P1/P2 |
| D-7 | Low | No CI/CD readiness signals: no workflows dir, no lint config, failing `npm test`, no release/versioning beyond `v1.0.0` strings in UI | Add §8.7 pipeline; gate merges on green parity suite | new infra | P2 |
| D-8 | Low | `vite.config.js` `open:true`, no preview-host policy; Google Fonts hard dependency in `index.html` (offline/Docker builds render with fallback only after flash) | `open:false`; self-host Fira Code/Inter via `@fontsource` | config | P3 |

---

## 10. Prioritized implementation roadmap

### Phase 1 — Correctness blockers (the compiler must be *right*)
1. Fix `json.c` compile error (C-1) and Makefile targets (C-9) — hours of work, unblocks everything.
2. Bytecode bounds checks (C-5) + checked allocs.
3. Parser: `#include` handling (C-2), diagnostics + panic recovery, unary/`++`/compound-assign, stop fabricating 0-literals (R-2).
4. TAC: real opcodes for comparisons/logical (C-3), `for` desugaring (C-4), `scanf`→READ (B-7); fix leaks/static buffers.
5. Semantic: link/free scopes, emit real diagnostics (B-6); JSON gains `success`/`diagnostics` (A-2).
6. VM: jumps/calls, format-aware printf, div-by-zero diagnostics, step cap (R-4).
7. Delete JS engine fakery: remove hardcoded VM fallbacks, `Function()` eval, regex diagnostics, fabricated metrics (C-7, C-8, C-10) — either implement the real subset in JS or adopt the WASM path (A-11) so there is one compiler.
8. Shared schema document (A-1).

### Phase 2 — Reliability and test coverage
9. Parity harness + golden programs (A-7), malformed-input corpus, optimizer-preservation property test (§8).
10. Rewrite `run_engine_test.mjs` expectations (F-2); make `npm test` and `make check` green; add ASan CI job.
11. Server hardening: Content-Length reads, timeouts, size caps, routing, `/api/health`, 127.0.0.1 default (B-4, B-5, A-3).
12. Frontend: proxy + engine badge + AbortController (F-1, A-4, A-6); remove all fake fallback values (F-3); input model without full recompile (F-4, A-5).

### Phase 3 — UX and learning features
13. Replace editor with CodeMirror, diagnostics gutter, click-diagnostic-to-line (R-9).
14. Real VM panels: call frames from trace, decoded instructions, breakpoints, step-to-cursor (R-8, A-9).
15. Optimizer pass toggles + TAC diff (A-8); language-subset reference panel (A-12).
16. Accessibility pass (F-6), responsive layout (F-7), localStorage/URL program persistence (A-10).

### Phase 4 — Production readiness
17. Multi-stage Docker image serving `dist/` + `nova_server` behind one origin with health checks and logging (D-1…D-4).
18. Full CI/CD: lint, unit, parity, e2e, Docker build; remove committed `dist/` (D-5, D-7).
19. Environment-driven config, docs refresh (README currently instructs commands that fail), optional rate limiting/auth if the server is ever exposed beyond localhost (D-3, D-6).

---

### Bottom line
The project's *presentation layer* is ahead of its *compiler substance*: six polished panels visualize data that, on inspection, is frequently simulated, miscompiled, or schema-incompatible between the two engines, and the native backend does not even build. The architecture itself (phase modules + JSON contract + visualizers + dual engines) is worth keeping. Execute Phase 1 — ideally converging on a single C implementation compiled to WASM for the browser — and NOVA can honestly claim deterministic, accurate compilation; until then the "compiler" label is generous.

---

## Addendum — accuracy hardening pass (2026-08-14)

Follow-up audit on branch `arena/019fffc8-nova-compiler` (base `cdc2602`).
Scope: execute the bundled GCC stress test (`tests/corpus/stress_test.c`),
then fuzz the compiler with simple programs and compare every console output
against real `gcc`. The stress test itself had 4 real defects (below); the
compiler had 9 accuracy gaps found by the comparison battery. All fixed in
**both** engines (JS and C99 mirror) and locked in by 13 new parity corpus
programs.

### Stress test fixes (`tests/corpus/stress_test.c`)

| # | Defect | Fix |
|---|---|---|
| 1 | `VECTOR_OP` macro used as an expression but expands to statements (`int result = VECTOR_OP(...)` fails to compile) | Rewrote as a GNU statement-expression `({ ... })` with unique locals |
| 2 | `int (^block)(int) = ^...` is Clang block syntax, not GCC | Replaced with a GCC nested function |
| 3 | Missing includes: `offsetof` needs `<stddef.h>`, `double complex`/`I` need `<complex.h>`, `errno` needs `<errno.h>` | Added includes (also moved `<signal.h>`/`<stdatomic.h>` to the top) |
| 4 | `setbuf(stdout, buffer)` with a function-local buffer → dangling pointer, **segfault at Test 10** | Made the buffer `static` |
| 5 | `fread` into uninitialized `read_buf` → `%s` may print garbage | Zero-initialized the buffer |
| 6 | Misleading "(should be 1.1)" comments on the float-accumulation lines | Clarified: 11 × 0.1f accumulated in float/double |

Verified: compiles cleanly with `gcc -std=gnu11 -O2 -Wall -Wextra`, runs all
12 suites, exit code 0. Every printed value was hand-checked; the two float
sums were reproduced bit-exactly in exact rational arithmetic
(`1.10000014305114746094` / `1.09999999999999986677`).

### Compiler gaps found and fixed (both engines)

| # | Symptom (simple program) | Root cause | Fix |
|---|---|---|---|
| 1 | `printf("%s", s)` on a hand-built `char` array printed garbage/empty | No array-to-pointer decay: `LOAD arr` pushed the *value of element 0* | Bare array identifiers decay to `&arr[0]` (`ADDR`); array params decay to the stored address |
| 2 | `char s[6] = "hello"` produced garbage | String-literal array init stored the string-pool *reference* into element 0 | Char-by-char copy (+NUL) into the array cells |
| 3 | `char s[] = "hello"` sized 1 → out-of-bounds at `s[4]` | Unsized-array inference counted 1 initializer child | `strlen+1` size for string-literal initializers |
| 4 | `int a[4] = {1,2}` in a re-entered function returned stale values (`99 99` vs gcc `99 0`) | Partial initializers never zero-filled the remainder | Explicit zero-fill of the remainder on every execution |
| 5 | `int find(int arr[], ...)` rejected | Array parameters unsupported | Parser/semantic/codegen support; indexing loads the address then adds the index |
| 6 | `p[1]` on `int *p = a` read `slot+1` instead of `a[1]` (`10 11 12` vs `10 20 30`) | Pointer indexing treated the pointer slot as the array base | Pointer/array-param indexing: load address, add index (no bounds check) |
| 7 | `scanf("%d %f %c %s", …)` silently wrote 0 for `%c`/`%s` | scanf parsed every line as a number | Per-conversion parsing: `%c` = first char of line, `%s` = line + NUL, numerics as before |
| 8 | `printf("%g", 1e20)` → `0`; `%.0f` of `2.5` differed between engines | `is_numeric_place` rejected scientific notation (`1e+20` treated as an identifier → `LOAD slot 0`); JS `toFixed` rounds ties away from zero (C: half-to-even); JS `%e`/`%g` didn't match C | Numeric-place regex accepts exponents; JS `%f`/`%e`/`%g` rewritten with exact BigInt decimal expansion, round-half-to-even, exponent zero-padding; C `%g` now uses real `snprintf` semantics |
| 9 | `char s[10]` sized 16 in the JS engine | `parseInt(sz.lexeme, 16)` — array sizes parsed as **hex** | `parseInt(..., 10)` (also for struct fields) |

Additional latent parity bug found while locking these in: JS `constString` /
trace `fmtNumber` used `String(v)` for large non-integral doubles while the C
backend emits shortest round-tripping `%g` (`1e20` vs `100000000000000000000`).
Unified through a shared `cFormatValue` helper.

### Follow-up struct gap (same session)

While probing simple programs, two more struct gaps surfaced and were fixed in
both engines:

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 10 | `struct Outer { struct Inner in; }` rejected ("struct fields are not supported"); `o.in.a` failed | Parser rejected `struct`-typed fields; member-address codegen only handled `id.field` | Parser accepts `struct` fields; `gen_member_addr` recurses through `NODE_MEMBER` bases |
| 11 | `struct P g = {7, 9};` (global) and local struct initializer lists were parse errors; nested flat init `{6,7,8}` stored to wrong offsets | Only arrays accepted `{...}` initializers; offsets were top-level only | Struct initializer lists with C's flat subobject mapping (leaf-offset flattening) + zero-fill of remaining leaves |

New parity corpus programs: `nested_struct`, `struct_init`, `struct_nested_init`.

### Verification

| Check | Result |
|---|---|
| `npm run test:engine` | 539 passed, 0 failed |
| `cd c_backend && make check` | 111 passed, 0 failed |
| `make asan && ./nova_native_test` | 111 passed, 0 failed |
| `npm run test:parity` | **945 fields compared, 0 mismatches** (63 programs, 16 new) |
| 31-program GCC comparison battery | 31/31 JS↔native parity; 30/31 match `gcc` byte-for-byte (the 1 remainder is the documented float32-rounding limitation: NOVA stores all floats as doubles) |

Known limitation (documented in `docs/SCHEMA.md`): there is no float32
precision — `float` variables behave as `double`, so constants that need
float32 rounding (`float c = 3.14e10;`) keep the full double value.

# NOVA Compiler Studio

An interactive compiler-learning platform that compiles a real C subset through a
full pipeline — lexer → parser → semantic analysis → three-address code →
optimizer → bytecode → stack VM — and visualizes every phase.

- **Frontend:** React 18 + Vite + Tailwind. One panel per compiler phase
  (tokens, AST, symbol table, TAC + optimizer metrics, bytecode, step-through
  VM visualizer, interactive console with `scanf` input).
- **Browser engine:** a complete compiler in `src/engine/compilerEngine.js`.
- **Native backend:** a faithful C99 mirror in `c_backend/`, exposed as an HTTP
  API. Both engines emit the **same JSON contract**, enforced by a parity test.

> This project was fully audited and rebuilt. See [`AUDIT.md`](AUDIT.md) for the
> findings and the fix roadmap, and [`docs/SCHEMA.md`](docs/SCHEMA.md) for the
> JSON contract and the supported language subset.

## Quick start

```bash
npm install
npm run dev          # http://localhost:3000  (browser engine by default)
```

The header has a toggle for **In-browser engine** vs **Native C backend**. In
native mode the UI calls `/api/compile`, which the Vite dev server proxies to
`nova_server` (default `http://127.0.0.1:8080`). If the native server is
unreachable the UI falls back to the browser engine and shows a visible notice.

### Start the native backend

```bash
npm run backend:build     # builds nova_compiler, nova_server, nova_native_test
npm run backend:server    # serves http://127.0.0.1:8080
```

Requires a C99 toolchain (gcc/clang + make). The server binds to `127.0.0.1`
by default; override with `NOVA_HOST` / `NOVA_PORT`.

## Tests

| Command | What it runs |
|---|---|
| `npm run test:engine` | 539 assertions on the browser engine (node, zero deps) |
| `npm run test:c-backend` | 111 assertions on the native backend (`make check`) |
| `npm run test:parity` | Deep-compares JS vs C JSON across 47 programs |
| `npm test` | engine + parity |
| `npm run test:all` | engine + native + parity |

The parity test is the key guarantee: for every program it compiles with both
engines and deep-compares the full result (tokens, AST, symbol table, TAC,
optimized TAC, metrics, bytecode, VM trace, console output, diagnostics). It
currently reports **720 fields compared, 0 mismatches**.

The native backend also builds clean under AddressSanitizer + UBSan
(`cd c_backend && make asan && ./nova_native_test`).

## Docker

Multi-stage build: compiles the frontend (`vite build`) and the native backend
(`make nova_server`, statically linked), then serves `dist/` with a small Node
server that also proxies `/api/*` to `nova_server` and exposes `/api/health`.

```bash
docker build -t nova-studio .
docker run --rm -p 3000:3000 nova-studio     # http://localhost:3000
```

## Repository layout

```
src/engine/compilerEngine.js   browser compiler (lexer→…→VM), reference impl
src/engine/presets.js          example programs for the UI
src/components/                phase visualizers + editor + console
c_backend/                     C99 mirror: lexer/parser/semantic/tac/optimizer/
                               bytecode/vm/json/compile + CLI + HTTP server
  main.c                       CLI:  nova_compiler file.c  (or `-` for stdin)
  server.c                     HTTP API server (nova_server)
  native_test.c                native test harness
tests/run_engine_test.mjs      browser-engine test suite
tests/parity_test.mjs          JS↔C parity suite
tests/corpus/stress_test.c     advanced C stress program (documented limits)
scripts/serve.mjs              production static + proxy server (Docker CMD)
docs/SCHEMA.md                 JSON contract + language subset
AUDIT.md                       audit findings + fix roadmap
```

## Supported language subset (v1)

Scalars (`int`, `char`, `float`, `double`), fixed arrays with initializer
lists, structs with member access, pointers (`&`, `*`, pointer parameters),
`if/else`, `while`, `do-while`, `for`, `switch/case/default` (with
fallthrough), `goto`/labels, `break`/`continue`, `return`, the ternary
operator, explicit casts, `sizeof(type)`, compound and bitwise assignments,
functions with recursion, and `printf`/`scanf` with format strings. A set of
math/runtime built-ins (`sqrt`, `pow`, `sin`, `cos`, `fabs`, `rand`, `srand`,
`assert`, `exit`, …) is provided.

Deliberately **not** supported (each produces a clear diagnostic): `typedef`,
`union`, `enum`, bitfields, function pointers, variadic functions, nested
function definitions, the C preprocessor beyond skipping directives, and
libc/heap/file I/O. See `docs/SCHEMA.md` for details.

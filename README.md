# NOVA Compiler Studio

An interactive compiler-learning platform for a real C subset language:

- **React 18 + Vite + Tailwind** frontend with one visualizer per compiler phase
  (tokens, AST, symbol table, TAC + optimizer, bytecode, step-through stack-VM,
  program console with `scanf` input).
- **Browser engine** — a complete compiler in `src/engine/compilerEngine.js`
  (lexer → recursive-descent parser → scoped semantic analysis + memory layout →
  three-address code → optimizer → bytecode → stack VM).
- **Native C99 backend** — the same pipeline implemented in `c_backend/`, exposed
  as an HTTP API (`POST /api/compile`, `GET /api/health`, `GET /api/version`).

Both engines are **1:1 mirrors**: given identical source and inputs they produce
identical JSON (verified field-by-field by `tests/parity_test.mjs`). No mocked
outputs, no fabricated metrics, no regex-based diagnostics.

See [`docs/SCHEMA.md`](docs/SCHEMA.md) for the full JSON contract and the language
subset, and [`AUDIT.md`](AUDIT.md) for the project audit and roadmap.

## Quick start (frontend + browser engine)

```bash
npm install
npm run dev          # http://localhost:3000
```

The UI works immediately with the in-browser engine. The "Native C backend" toggle
routes compilation through the C server when it is reachable; otherwise the UI
falls back to the browser engine and shows a visible notice.

## Native C backend

Requires a C99 compiler (gcc/clang) and make.

```bash
npm run backend:build        # builds nova_compiler, nova_server, nova_native_test
npm run backend:server       # serves http://127.0.0.1:8080
```

Environment variables: `NOVA_HOST` (default `127.0.0.1` — deliberately not exposed
to the network by default), `NOVA_PORT` (default `8080`).

With both running, `npm run dev` proxies `/api/*` to the backend automatically
(target can be overridden with `NOVA_BACKEND_URL`).

CLI usage:

```bash
c_backend/nova_compiler program.c            # prints the JSON contract
echo 'int main(){return 0;}' | c_backend/nova_compiler -
NOVA_INPUTS=$'42\n7' c_backend/nova_compiler program.c   # scanf inputs
```

## Tests

```bash
npm run test:engine      # 327 assertions on the browser engine (node, no deps)
npm run test:c-backend   # 79 assertions on the C backend (make check)
npm run test:parity      # deep-compares JS vs C output over a 47-program corpus
npm test                 # engine + parity (parity auto-builds the C binary,
                         # or skips with a message when no toolchain exists)
npm run test:all         # everything
```

## Docker (production)

Multi-stage build: compiles the frontend (`vite build`), compiles the native
backend (`make nova_server`, static), and runs a small Node server that serves
`dist/` and proxies `/api/*` to the supervised `nova_server`:

```bash
docker build -t nova-studio .
docker run --rm -p 3000:3000 nova-studio    # http://localhost:3000
```

A `HEALTHCHECK` is included (`GET /api/health` through the proxy).

## Repository layout

```
src/engine/compilerEngine.js   browser compiler (reference implementation)
src/engine/presets.js          example programs used by the UI
src/components/                phase visualizers
c_backend/                     C99 mirror: lexer, parser, semantic, tac,
                               optimizer, bytecode, vm, json, compile pipeline,
                               CLI (main.c), HTTP server (server.c), tests
tests/                         engine test suite + JS<->C parity suite
scripts/serve.mjs              production static + proxy server (Docker CMD)
docs/SCHEMA.md                 JSON contract + language subset
```

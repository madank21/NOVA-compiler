"# NOVA-compiler

## Overview

NOVA is a hybrid learning project combining:

- A React + Vite frontend UI for viewing compiler phases.
- A browser-side JavaScript compiler engine in `src/engine/compilerEngine.js`.
- A native C backend compiler under `c_backend/` for a more realistic pipeline.

## Local usage

Install Node dependencies:

```bash
npm install
```

Run the web app locally:

```bash
npm run dev
```

Build the frontend:

```bash
npm run build
```

Run the browser engine tests:

```bash
npm test
```

Run the native C backend test harness:

```bash
npm run test:c-backend
```

## Native C backend

If you want to compile the native backend, install a C toolchain first.

### Windows (MSYS2 / MinGW)

1. Install MSYS2 from https://www.msys2.org
2. Open the MSYS2 MinGW shell and install GCC:

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc
```

3. Build from `c_backend/`:

```bash
cd c_backend
make
```

4. Run the compiler or server:

```bash
./nova_compiler
./nova_server
```

### WSL / Linux

```bash
cd c_backend
make
```

## Browser fallback

The app attempts to use the native backend at `http://localhost:8080/api/compile` when backend mode is set to `native`.
If the server is not reachable, it falls back to the built-in browser JS compiler engine.

## Notes

- `tests/run_engine_test.mjs` exercises the browser compiler engine and validates diagnostics for malformed code.
- The frontend now displays diagnostics in the console panel when compilation fails.
" 

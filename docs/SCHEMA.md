# NOVA Compiler JSON Contract (v2)

Both engines — the browser implementation (`src/engine/compilerEngine.js`) and the
native C backend (`c_backend/`) — emit **byte-identical JSON** for the same source
and inputs. This is enforced by `tests/parity_test.mjs` (field-by-field deep
comparison; only `compile_time_ms` and `engine` are exempt).

## Request

`POST /api/compile` (native backend or the Vite/Docker proxy):

```json
{ "source": "int main() { return 0; }", "inputs": ["42"] }
```

- `source` (string, required): the C-subset program.
- `inputs` (array of strings, optional): stdin values consumed by `scanf` in order.

A raw `text/plain` body is also accepted and treated as `source`. The CLI mirrors
this: `nova_compiler file.c` with `NOVA_INPUTS` (newline-separated) for scanf input.

## Response

| Field | Type | Description |
|---|---|---|
| `success` | boolean | true when no `error`-level diagnostics exist (compile **and** runtime) |
| `engine` | string | `"browser-js"` or `"native-c"` |
| `compile_time_ms` | number | pipeline wall time (excluded from parity comparisons) |
| `tokens` | `Token[]` | full token stream including `TOKEN_EOF` |
| `ast` | `AstNode` | children-only abstract syntax tree |
| `symbolTable` | `Symbol[]` | insertion-order table: builtins, structs, globals, functions, then per-function scopes |
| `tac` | `TacInstr[]` | raw three-address code |
| `optTac` | `TacInstr[]` | optimized TAC |
| `metrics` | `Metrics` | **real** optimizer counters |
| `bytecode` | `Instr[]` | stack-VM bytecode with resolved jump targets |
| `vmTrace` | `Step[]` | one step per executed instruction (capped at 2000, see `vmTraceTruncated`) |
| `vmTraceTruncated` | boolean | trace was capped |
| `waitingForInput` | boolean | execution suspended at `scanf` awaiting more `inputs` |
| `inputPrompt` | string | human-readable prompt while suspended |
| `consoleOutput` | string | final program stdout |
| `exitCode` | number | `main`'s return value |
| `diagnostics` | `Diag[]` | compile + runtime diagnostics |

### Token

```json
{ "type": "TOKEN_INT", "lexeme": "int", "line": 1, "column": 1 }
```

`type` is one of the C-backend token names (`TOKEN_INT`, `TOKEN_PLUS`,
`TOKEN_INTEGER_LITERAL`, `TOKEN_STRING_LITERAL`, `TOKEN_INCLUDE`, `TOKEN_EOF`, …).
The same enum is used by both engines.

### AstNode

```json
{ "type": "NODE_BINARY_OP", "op": "+", "line": 3, "children": [ … ] }
```

Optional fields (present only when meaningful): `identifier`, `type_name`, `op`,
`num_val`, `string_val`, `is_array` (true), `has_size` (true when an explicit array
size literal is present — `children[0]` is then that size literal).

Node types: `NODE_PROGRAM`, `NODE_FUNCTION_DEF`, `NODE_PARAMETER`,
`NODE_STRUCT_DEF`, `NODE_STRUCT_FIELD`, `NODE_VAR_DECL`, `NODE_DECL_LIST`,
`NODE_IF_STMT`, `NODE_WHILE_STMT`, `NODE_FOR_STMT`, `NODE_RETURN_STMT`,
`NODE_BREAK_STMT`, `NODE_CONTINUE_STMT`, `NODE_COMPOUND_STMT`,
`NODE_EXPRESSION_STMT`, `NODE_BINARY_OP`, `NODE_UNARY_OP` (`op` `-` `!` `*` `&`
`++` `--` `p++` `p--`), `NODE_ASSIGNMENT`, `NODE_COMPOUND_ASSIGN`,
`NODE_FUNC_CALL`, `NODE_INDEX`, `NODE_MEMBER`, `NODE_INT_LITERAL`,
`NODE_FLOAT_LITERAL`, `NODE_STRING_LITERAL`, `NODE_IDENTIFIER`, `NODE_EMPTY`,
`NODE_ERROR`.

### Symbol

```json
{ "scope": "main", "name": "x", "kind": "Variable", "type": "int",
  "address": "0x0014", "params": 0 }
```

- `kind`: `Function` | `Variable` | `Parameter` | `Array` | `Struct`
- `address`: `0x` + slot*4 (uppercase hex); frame slots are relative to the frame base
- `params`: arity, or `-1` for variadic builtins (`printf`, `scanf`)
- Redeclared names (e.g. `i` in two sibling `for` loops) appear once per declaration
  in the table; the VM/bytecode resolve to the **latest** declaration.

### TacInstr

```json
{ "op": "+", "res": "t3", "a1": "t1", "a2": "t2", "line": 4 }
```

`res` = write target, `a1`/`a2` = read operands. Opcodes:

| Category | Opcodes |
|---|---|
| arithmetic/logic | `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=` `&&` `||` `neg` `!` |
| assignment | `=` (res = a1) |
| memory | `ADDR` (res = &a1 + a2), `IDX_ADDR` (res = &a1[a2]), `LOAD_PTR` (res = *a1), `STORE_PTR` (*a1 = a2) |
| control | `LABEL`, `GOTO`, `IF_FALSE` |
| calls | `PARAM`, `CALL` (res = call a1, a2 args), `RETURN` (a1 = value) |
| io | `PRINT` (a1 = format string ref, a2 = arg count), `READ` (scanf) |
| structure | `FUNC_BEGIN`, `FUNC_END` |

Temporaries are `t0…`, labels `L0…` — counters are shared across the whole program,
so numbering is identical in both engines.

### Metrics

```json
{ "constant_fold": 1, "constant_prop": 1, "dead_code": 1,
  "strength_reduce": 0, "reduction_percentage": 12.5 }
```

All counters are real rewrite counts. `reduction_percentage =
round((1 - optTac.length / tac.length) * 1000) / 10` — never fabricated.

### Bytecode Instr

```json
{ "pc": 3, "op": "STORE", "operand": 0, "symbol": "x", "line": 2 }
```

Opcodes: `PUSH`, `PUSH_STR`, `POP`, `LOAD`, `STORE`, `ADDR`, `IDX_ADDR`,
`LOAD_AT`, `STORE_AT`, `ADD` `SUB` `MUL` `DIV` (truncating int) `DIVF` (float)
`MOD`, `NEG`, `NOT`, `EQ` `NEQ` `LT` `GT` `LEQ` `GEQ`, `AND` `OR`, `JMP`, `JZ`,
`CALL`, `RET`, `PRINT`, `INPUT`, `HALT`.

`JMP`/`JZ` operands are resolved instruction indices. `IDX_ADDR` performs bounds
checking for fixed-size arrays at runtime.

### VM Step

```json
{ "step": 5, "pc": 5, "line": 3, "instruction": "STORE x",
  "stack": [14], "variables": [{ "name": "x", "value": 14 }],
  "frames": [{ "func": "main()", "retAddr": "0x0000" }],
  "console": "" }
```

- `instruction` is the decoded opcode + operand (never a fake `EXEC_LINE`).
- `stack` is the operand stack before the instruction executes (string-pool cells
  serialize as `0`).
- `variables` = globals + current frame locals (Map semantics: one entry per name,
  latest declaration wins).
- `frames` = full call stack, deepest first.

### Diag

```json
{ "level": "error", "msg": "Undefined identifier 'y'", "line": 1, "column": 1 }
```

`level`: `error` (compile), `warning`, or `runtime` (division by zero, out-of-bounds
index, invalid memory access, call-stack overflow, step-limit exceeded). Runtime
diagnostics halt execution and set `success` to `false` at the pipeline level only
when they occur before VM execution completes normally; the JSON keeps both.

## Execution limits (both engines)

| Limit | Value |
|---|---|
| VM step cap | 200 000 |
| Recorded trace steps | 2 000 |
| Operand stack | 4 096 cells |
| Call depth | 1 024 frames |
| Memory | 65 536 double-word slots |

## Language subset (v1)

`int`, `char`, `float`, `double` scalars; fixed arrays with initializer lists;
struct definitions and member access; pointers (`&`, `*`, pointer parameters);
`if/else`, `while`, `do/while`, `for`, `switch`/`case`/`default`, `goto`/labels,
`break`, `continue`, `return`, ternary `?:`, casts, and `sizeof`; functions with
recursion; `printf`/`scanf` with format strings; global and static variables.
Preprocessor directives are skipped (`#include`, `#define`) with basic
conditional-directive handling. Deliberately unsupported constructs produce a
clear diagnostic: `typedef`, unions, enums, bitfields, function pointers,
variadic or nested function definitions, dynamic allocation, and libc file I/O.

### Semantics notes (v1, enforced by the parity suite)

- **Array-to-pointer decay.** A bare array name in an expression is the address
  of its first element (`&arr[0]`), exactly like C. This makes `printf("%s",
  s)` on a `char` array, `int *p = a;`, and passing arrays to functions work.
- **Array parameters.** `int arr[]` (and `int arr[5]`) parameter declarations
  are accepted; the parameter holds the caller's address. Indexing an array
  parameter or a pointer (`p[i]`) performs address arithmetic without bounds
  checking (C pointer semantics); fixed arrays keep runtime bounds checks.
  Addresses are plain numbers, so `p + k` / `p - k` also work with numeric
  (unit-element) semantics.
- **String-literal array initializers.** `char s[6] = "hello"` (and unsized
  `char s[] = "hello"`, sized `strlen+1`) copies the characters plus NUL into
  the array cells. Partial initializer lists (`int a[4] = {1, 2}`) zero-fill
  the remainder every time the declaration executes (C semantics).
- **Structs nest and initialize.** Struct-typed fields (`struct Inner in;`),
  member chains (`o.in.a`), and initializer lists for globals and locals
  (`struct P g = {7, 9};`) are supported with C's flat subobject mapping:
  `struct C c = {6, 7, 8}` fills the innermost fields first. Partial struct
  initializer lists zero-fill the remaining leaves, and a string initializer
  consumes its complete array field (`struct S s = {"hi", 3}`). Array members
  decay to their first-element address, so `s.text`, `s.values[i]`, and
  addresses of scalar members (`&s.value`) have C semantics.
- **Lexical shadowing.** Block/function locals are bound to unique internal
  storage slots. Inner declarations correctly shadow outer locals and globals,
  and the outer binding is restored when its scope ends.
- **Value types survive code generation.** Double array/pointer elements and
  double-returning user or math functions retain their type, ensuring `/` uses
  floating-point rather than truncating integer division.
- **Types are stored as doubles.** `char`/`float` normalize to `int`/`double`;
  there is no float32 rounding of constants or arithmetic (a documented
  limitation — e.g. `float` literals like `3.14e10` are not rounded to float32).
- **Scientific notation** (`1e20`, `1.5e-3`) is supported in literals.
- **printf rounding matches C** (`glibc`): `%f`/`%e` round half-to-even on the
  exact binary value, `%g`/`%G` use C's precision-significant-digit rules with
  trailing zeros stripped, and exponents are zero-padded to two digits.
- **scanf conversions** consume one input line each; `%c` takes the line's
  first character, `%s` copies the line (plus NUL) into the target address.

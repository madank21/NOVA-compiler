// ============================================================================
// NOVA Studio Compiler Engine (browser implementation)
// ----------------------------------------------------------------------------
// A real, deterministic compiler pipeline for the NOVA C subset:
//   lexer -> parser -> semantic analysis (scopes/layout) -> TAC ->
//   optimizer (fold / const-prop / strength / DCE) -> bytecode -> stack VM
//
// This engine is mirrored 1:1 by the native C backend (c_backend/). Both
// produce the same JSON contract documented in docs/SCHEMA.md, which is
// enforced by tests/parity_test.mjs. There are NO mocked outputs, NO regex
// diagnostics and NO hardcoded program results in this file.
//
// Language subset (v1): int/char/float/double scalars, fixed arrays, structs
// (definition + member access), pointers (& * and pointer parameters),
// if/else, while, for, break, continue, return, functions with recursion,
// printf/scanf with format strings.
// ============================================================================

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------

const KEYWORDS = {
  auto: 'TOKEN_AUTO', break: 'TOKEN_BREAK', case: 'TOKEN_CASE', char: 'TOKEN_CHAR',
  const: 'TOKEN_CONST', continue: 'TOKEN_CONTINUE', default: 'TOKEN_DEFAULT', do: 'TOKEN_DO',
  double: 'TOKEN_DOUBLE', else: 'TOKEN_ELSE', enum: 'TOKEN_ENUM', extern: 'TOKEN_EXTERN',
  float: 'TOKEN_FLOAT', for: 'TOKEN_FOR', goto: 'TOKEN_GOTO', if: 'TOKEN_IF',
  int: 'TOKEN_INT', long: 'TOKEN_LONG', register: 'TOKEN_REGISTER', return: 'TOKEN_RETURN',
  short: 'TOKEN_SHORT', signed: 'TOKEN_SIGNED', sizeof: 'TOKEN_SIZEOF', static: 'TOKEN_STATIC',
  struct: 'TOKEN_STRUCT', switch: 'TOKEN_SWITCH', typedef: 'TOKEN_TYPEDEF', union: 'TOKEN_UNION',
  unsigned: 'TOKEN_UNSIGNED', void: 'TOKEN_VOID', volatile: 'TOKEN_VOLATILE', while: 'TOKEN_WHILE'
};

const TYPE_TOKENS = new Set(['TOKEN_INT', 'TOKEN_FLOAT', 'TOKEN_DOUBLE', 'TOKEN_CHAR', 'TOKEN_VOID']);

const VM_MAX_STEPS = 200000;
const TRACE_MAX_STEPS = 2000;
const MEM_MAX = 65536;
const STACK_MAX = 4096;
const CALL_DEPTH_MAX = 1024;

// ---------------------------------------------------------------------------
// Number formatting helpers (must match the C backend byte-for-byte)
// ---------------------------------------------------------------------------

function isIntegral(v) {
  return Number.isFinite(v) && v === Math.trunc(v) && Math.abs(v) < 1e15;
}

function truncateToInteger(v) {
  // C-style cast to long long (toward zero), bounded for JS safety
  if (!Number.isFinite(v)) return 0;
  let t = Math.trunc(v);
  const LIM = 9007199254740991; // 2^53 - 1
  if (t > LIM) t = LIM;
  if (t < -LIM) t = -LIM;
  return t;
}

// ---------------------------------------------------------------------------
// Predefined macros & constants (identical in the JS and C engines)
// ---------------------------------------------------------------------------

// Object-like macros usable in #if/#ifdef and as values.
const PREDEFINED_MACROS = {
  '__STDC__': { kind: 'int', value: 1 },
  '__STDC_VERSION__': { kind: 'int', value: 199901 },
  '__STDC_HOSTED__': { kind: 'int', value: 1 },
  '__STDC_NO_ATOMICS__': { kind: 'int', value: 1 },
  '__NOVA__': { kind: 'int', value: 1 },
  '__VERSION__': { kind: 'string', value: 'NOVA 2.0' },
  '__FILE__': { kind: 'string', value: 'main.c' },
  'NULL': { kind: 'int', value: 0 },
  'M_PI': { kind: 'float', value: 3.141592653589793 },
  'M_E': { kind: 'float', value: 2.718281828459045 }
};

const DIAGNOSTIC_LIMIT = 100;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

class DiagList {
  constructor() { this.items = []; this.limitNoteAdded = false; }
  add(level, line, column, msg) {
    if (this.items.length >= DIAGNOSTIC_LIMIT) {
      if (!this.limitNoteAdded) {
        this.limitNoteAdded = true;
        this.items.push({
          level: 'warning', line: 0, column: 0,
          msg: `Diagnostic limit (${DIAGNOSTIC_LIMIT}) reached — further diagnostics suppressed. ` +
               'The program likely uses constructs outside the NOVA C subset.'
        });
      }
      return;
    }
    this.items.push({ level, msg, line, column });
  }
  hasErrors() { return this.items.some((d) => d.level === 'error'); }
}

// ---------------------------------------------------------------------------
// Phase 0/1: Preprocessor conditionals + Lexer
// ---------------------------------------------------------------------------

// Minimal #if expression evaluator: defined(X), defined X, integer literals,
// identifiers (0), ! && || ( ) and comparison operators. Deterministic.
function evalPreprocExpr(text) {
  let i = 0;
  const n = text.length;

  const skipWs = () => { while (i < n && /\s/.test(text[i])) i++; };

  function parsePrimary() {
    skipWs();
    if (i >= n) return 0;
    if (text[i] === '(') {
      i++;
      const v = parseOr();
      skipWs();
      if (text[i] === ')') i++;
      return v;
    }
    if (text[i] === '!') {
      i++;
      return parsePrimary() ? 0 : 1;
    }
    if (/[0-9]/.test(text[i])) {
      let s = '';
      if (text[i] === '0' && (text[i + 1] === 'x' || text[i + 1] === 'X')) {
        s += text[i++]; s += text[i++];
        while (i < n && /[0-9a-fA-F]/.test(text[i])) s += text[i++];
        return parseInt(s, 16) || 0;
      }
      while (i < n && /[0-9]/.test(text[i])) s += text[i++];
      if (i < n && (text[i] === 'L' || text[i] === 'l' || text[i] === 'U' || text[i] === 'u')) i++;
      return parseInt(s, 10) || 0;
    }
    if (text.startsWith('defined', i) && (i + 7 >= n || !/[a-zA-Z0-9_]/.test(text[i + 7]))) {
      i += 7;
      skipWs();
      let paren = false;
      if (text[i] === '(') { paren = true; i++; }
      skipWs();
      let name = '';
      while (i < n && /[a-zA-Z0-9_]/.test(text[i])) name += text[i++];
      skipWs();
      if (paren && text[i] === ')') i++;
      return Object.prototype.hasOwnProperty.call(PREDEFINED_MACROS, name) ? 1 : 0;
    }
    if (/[a-zA-Z_]/.test(text[i])) {
      let name = '';
      while (i < n && /[a-zA-Z0-9_]/.test(text[i])) name += text[i++];
      if (Object.prototype.hasOwnProperty.call(PREDEFINED_MACROS, name)) {
        const m = PREDEFINED_MACROS[name];
        return m.kind === 'int' ? m.value : 1;
      }
      return 0;
    }
    i++;
    return 0;
  }

  function parseRel() {
    let v = parsePrimary();
    for (;;) {
      skipWs();
      const two = text.slice(i, i + 2);
      const one = text[i];
      let op = null;
      if (two === '<=' || two === '>=' || two === '==' || two === '!=') { op = two; i += 2; }
      else if (one === '<' || one === '>') { op = one; i += 1; }
      else break;
      const r = parsePrimary();
      if (op === '<=') v = v <= r ? 1 : 0;
      else if (op === '>=') v = v >= r ? 1 : 0;
      else if (op === '==') v = v === r ? 1 : 0;
      else if (op === '!=') v = v !== r ? 1 : 0;
      else if (op === '<') v = v < r ? 1 : 0;
      else v = v > r ? 1 : 0;
    }
    return v;
  }

  function parseAnd() {
    let v = parseRel();
    for (;;) {
      skipWs();
      if (text.startsWith('&&', i)) { i += 2; const r = parseRel(); v = (v && r) ? 1 : 0; }
      else break;
    }
    return v;
  }

  function parseOr() {
    let v = parseAnd();
    for (;;) {
      skipWs();
      if (text.startsWith('||', i)) { i += 2; const r = parseAnd(); v = (v || r) ? 1 : 0; }
      else break;
    }
    return v;
  }

  return parseOr() ? 1 : 0;
}

function tokenize(source, diags) {
  const tokens = [];
  let pos = 0, line = 1, col = 1;
  const n = source.length;

  // Conditional-compilation stack: {active, everActive, parentActive}
  const condStack = [];
  const regionActive = () => condStack.every((e) => e.active);
  const parentActive = () => condStack.length === 0 ? true : condStack[condStack.length - 1].active;

  const handleConditional = (word, rest, l, c) => {
    if (word === 'ifdef' || word === 'ifndef') {
      const name = rest.trim().split(/\s+/)[0] || '';
      const defined = Object.prototype.hasOwnProperty.call(PREDEFINED_MACROS, name);
      const cond = word === 'ifdef' ? defined : !defined;
      const parent = parentActive();
      condStack.push({ active: parent && cond, everActive: parent && cond, parentActive: parent });
    } else if (word === 'if') {
      const parent = parentActive();
      const cond = parent ? evalPreprocExpr(rest) !== 0 : false;
      condStack.push({ active: cond, everActive: cond, parentActive: parent });
    } else if (word === 'elif') {
      if (condStack.length === 0) {
        diags.add('error', l, c, '#elif without #if');
        return;
      }
      const top = condStack[condStack.length - 1];
      if (top.everActive || !top.parentActive) {
        top.active = false;
      } else {
        top.active = evalPreprocExpr(rest) !== 0;
        if (top.active) top.everActive = true;
      }
    } else if (word === 'else') {
      if (condStack.length === 0) {
        diags.add('error', l, c, '#else without #if');
        return;
      }
      const top = condStack[condStack.length - 1];
      top.active = top.parentActive && !top.everActive;
      if (top.active) top.everActive = true;
    } else if (word === 'endif') {
      if (condStack.length === 0) {
        diags.add('error', l, c, '#endif without #if');
        return;
      }
      condStack.pop();
    }
  };

  const peek = (o = 0) => (pos + o < n ? source[pos + o] : '\0');
  const advance = () => {
    const c = source[pos++];
    if (c === '\n') { line++; col = 1; } else { col++; }
    return c;
  };
  const push = (type, lexeme, l, c, extra) => {
    tokens.push(Object.assign({ type, lexeme, line: l, column: c }, extra || {}));
  };

  while (pos < n) {
    const c = peek();

    // Translation phase 2: backslash-newline line splicing
    if (c === '\\' && (peek(1) === '\n' || (peek(1) === '\r' && peek(2) === '\n'))) {
      advance(); // backslash (does not count as a line break)
      if (peek() === '\r') advance();
      // consume the newline without incrementing the logical line count
      pos++;
      continue;
    }

    if (c === '\n' || c === ' ' || c === '\t' || c === '\r' || c === '\v' || c === '\f') {
      advance();
      continue;
    }

    // Comments
    if (c === '/' && peek(1) === '/') {
      while (pos < n && peek() !== '\n') advance();
      continue;
    }
    if (c === '/' && peek(1) === '*') {
      advance(); advance();
      let closed = false;
      while (pos < n) {
        if (peek() === '*' && peek(1) === '/') { advance(); advance(); closed = true; break; }
        advance();
      }
      if (!closed) diags.add('error', line, col, 'Unterminated block comment');
      continue;
    }

    const startLine = line, startCol = col;

    // Preprocessor directives
    if (c === '#') {
      advance();
      let word = '';
      while (pos < n && /[a-zA-Z]/.test(peek())) word += advance();

      // conditional-compilation directives are handled by the lexer itself
      if (word === 'ifdef' || word === 'ifndef' || word === 'if' ||
          word === 'elif' || word === 'else' || word === 'endif') {
        let rest = '';
        while (pos < n && peek() !== '\n') rest += advance();
        handleConditional(word, rest, startLine, startCol);
        continue;
      }

      // other directives emit tokens; the parser skips to end of line.
      // (Suppressed inside inactive conditional regions.)
      if (!regionActive()) {
        while (pos < n && peek() !== '\n') advance();
        continue;
      }
      const full = '#' + word;
      if (full === '#include') push('TOKEN_INCLUDE', full, startLine, startCol);
      else if (full === '#define') push('TOKEN_DEFINE', full, startLine, startCol);
      else push('TOKEN_HASH', full || '#', startLine, startCol);
      continue;
    }

    // Everything below is suppressed inside inactive conditional regions
    if (!regionActive()) {
      advance();
      continue;
    }

    // Identifiers / keywords / predefined macros
    if (/[a-zA-Z_]/.test(c)) {
      let lex = '';
      while (pos < n && /[a-zA-Z0-9_]/.test(peek())) lex += advance();
      // GNU-style attributes are ignored: __attribute__((...))
      if (lex === '__attribute__') {
        // skip balanced parentheses
        if (peek() === '(') {
          let depth = 0;
          while (pos < n) {
            const ch = peek();
            if (ch === '(') depth++;
            else if (ch === ')') { depth--; if (depth === 0) { advance(); break; } }
            else if (ch === '\n') break;
            advance();
          }
        }
        continue;
      }
      if (Object.prototype.hasOwnProperty.call(PREDEFINED_MACROS, lex)) {
        const m = PREDEFINED_MACROS[lex];
        if (m.kind === 'int') {
          push('TOKEN_INTEGER_LITERAL', String(m.value), startLine, startCol);
        } else if (m.kind === 'float') {
          push('TOKEN_FLOAT_LITERAL', String(m.value), startLine, startCol);
        } else {
          const t = { type: 'TOKEN_STRING_LITERAL', lexeme: `"${m.value}"`, line: startLine, column: startCol };
          t.stringValue = m.value;
          tokens.push(t);
        }
        continue;
      }
      push(KEYWORDS[lex] || 'TOKEN_IDENTIFIER', lex, startLine, startCol);
      continue;
    }

    // Numbers (decimal, hex, float with . and exponent)
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(peek(1)))) {
      let lex = '';
      let isFloat = false;
      if (c === '0' && (peek(1) === 'x' || peek(1) === 'X')) {
        lex += advance(); lex += advance();
        while (pos < n && /[0-9a-fA-F]/.test(peek())) lex += advance();
        while (pos < n && /[fFlLuU]/.test(peek())) advance(); // suffixes (LL, ULL, ...)
        push('TOKEN_INTEGER_LITERAL', lex, startLine, startCol);
        continue;
      }
      while (pos < n && /[0-9]/.test(peek())) lex += advance();
      if (peek() === '.' && /[0-9]/.test(peek(1))) {
        isFloat = true;
        lex += advance();
        while (pos < n && /[0-9]/.test(peek())) lex += advance();
      } else if (peek() === '.' && lex.length > 0 && !/[eE]/.test(peek(1)) && peek(1) !== '.') {
        // e.g. `3.` or `3.f` style — treat trailing dot as part of literal only if followed by non-ident
        if (!/[a-zA-Z_]/.test(peek(1))) { isFloat = true; lex += advance(); }
      }
      if (peek() === 'e' || peek() === 'E') {
        const save = pos, saveL = line, saveC = col;
        let exp = advance();
        if (peek() === '+' || peek() === '-') exp += advance();
        if (/[0-9]/.test(peek())) {
          isFloat = true;
          lex += exp;
          while (pos < n && /[0-9]/.test(peek())) lex += advance();
        } else {
          pos = save; line = saveL; col = saveC;
        }
      }
      // suffixes: consume any combination of l/L/u/U/f/F (e.g. 0xFFLL, 3u)
      while (pos < n && /[fFlLuU]/.test(peek())) advance();
      push(isFloat ? 'TOKEN_FLOAT_LITERAL' : 'TOKEN_INTEGER_LITERAL', lex, startLine, startCol);
      continue;
    }

    // Character literals
    if (c === "'") {
      advance();
      let value = 0;
      let body = '';
      let closed = false;
      while (pos < n && peek() !== "'" && peek() !== '\n') {
        let ch = advance();
        if (ch === '\\' && pos < n) {
          const esc = advance();
          if (esc === 'n') value = 10;
          else if (esc === 't') value = 9;
          else if (esc === '0') value = 0;
          else if (esc === 'r') value = 13;
          else if (esc === '\\') value = 92;
          else if (esc === "'") value = 39;
          else value = esc.charCodeAt(0);
          body += '\\' + esc;
        } else {
          value = ch.charCodeAt(0);
          body += ch;
        }
      }
      if (pos < n && peek() === "'") { advance(); closed = true; }
      if (!closed) diags.add('error', startLine, startCol, 'Unterminated character literal');
      push('TOKEN_CHAR_LITERAL', `'${body}'`, startLine, startCol);
      tokens[tokens.length - 1].charValue = value;
      continue;
    }

    // String literals
    if (c === '"') {
      advance();
      let value = '';
      let lex = '"';
      let closed = false;
      while (pos < n && peek() !== '"' && peek() !== '\n') {
        let ch = advance();
        if (ch === '\\' && pos < n) {
          const esc = advance();
          if (esc === 'n') value += '\n';
          else if (esc === 't') value += '\t';
          else if (esc === '0') value += '\0';
          else if (esc === 'r') value += '\r';
          else value += esc;
          lex += '\\' + esc;
        } else {
          value += ch;
          lex += ch;
        }
      }
      if (pos < n && peek() === '"') { advance(); lex += '"'; closed = true; }
      if (!closed) diags.add('error', startLine, startCol, 'Unterminated string literal');
      push('TOKEN_STRING_LITERAL', lex, startLine, startCol);
      tokens[tokens.length - 1].stringValue = value;
      continue;
    }

    // Ellipsis (variadic parameters) — must precede single-char '.' handling
    if (c === '.' && peek(1) === '.' && peek(2) === '.') {
      advance(); advance(); advance();
      push('TOKEN_ELLIPSIS', '...', startLine, startCol);
      continue;
    }

    // Multi-character operators (3-char first)
    const three = c + peek(1) + peek(2);
    const THREE_CHAR = {
      '<<=': 'TOKEN_LSHIFT_ASSIGN', '>>=': 'TOKEN_RSHIFT_ASSIGN'
    };
    if (THREE_CHAR[three]) {
      advance(); advance(); advance();
      push(THREE_CHAR[three], three, startLine, startCol);
      continue;
    }
    const two = c + peek(1);
    const TWO_CHAR = {
      '++': 'TOKEN_PLUS_PLUS', '--': 'TOKEN_MINUS_MINUS', '==': 'TOKEN_EQ', '!=': 'TOKEN_NEQ',
      '<=': 'TOKEN_LEQ', '>=': 'TOKEN_GEQ', '&&': 'TOKEN_AND', '||': 'TOKEN_OR',
      '+=': 'TOKEN_PLUS_ASSIGN', '-=': 'TOKEN_MINUS_ASSIGN', '*=': 'TOKEN_STAR_ASSIGN',
      '/=': 'TOKEN_SLASH_ASSIGN', '%=': 'TOKEN_PERCENT_ASSIGN', '->': 'TOKEN_ARROW',
      '<<': 'TOKEN_LSHIFT', '>>': 'TOKEN_RSHIFT',
      '&=': 'TOKEN_AND_ASSIGN', '|=': 'TOKEN_OR_ASSIGN', '^=': 'TOKEN_XOR_ASSIGN'
    };
    if (TWO_CHAR[two]) {
      advance(); advance();
      push(TWO_CHAR[two], two, startLine, startCol);
      continue;
    }

    // Single-character tokens
    const ONE_CHAR = {
      '+': 'TOKEN_PLUS', '-': 'TOKEN_MINUS', '*': 'TOKEN_STAR', '/': 'TOKEN_SLASH',
      '%': 'TOKEN_PERCENT', '=': 'TOKEN_ASSIGN', '!': 'TOKEN_NOT', '<': 'TOKEN_LT',
      '>': 'TOKEN_GT', '&': 'TOKEN_AMPERSAND', '|': 'TOKEN_BIT_OR', '^': 'TOKEN_BIT_XOR',
      '~': 'TOKEN_BIT_NOT', '?': 'TOKEN_QUESTION', ':': 'TOKEN_COLON', '(': 'TOKEN_LPAREN',
      ')': 'TOKEN_RPAREN', '{': 'TOKEN_LBRACE', '}': 'TOKEN_RBRACE', '[': 'TOKEN_LBRACKET',
      ']': 'TOKEN_RBRACKET', ';': 'TOKEN_SEMICOLON', ',': 'TOKEN_COMMA', '.': 'TOKEN_DOT'
    };
    if (ONE_CHAR[c]) {
      advance();
      push(ONE_CHAR[c], c, startLine, startCol);
      continue;
    }

    advance();
    diags.add('error', startLine, startCol, `Unexpected character '${c}'`);
    push('TOKEN_ERROR', c, startLine, startCol);
  }

  while (condStack.length > 0) {
    condStack.pop();
    diags.add('error', line, col, 'Unterminated #if/#ifdef (missing #endif)');
  }

  push('TOKEN_EOF', 'EOF', line, col);
  return tokens;
}

// ---------------------------------------------------------------------------
// Phase 2: Parser (recursive descent, children-only AST)
// ---------------------------------------------------------------------------

function makeNode(type, line) {
  return { type, line, children: [] };
}

function parseProgram(tokens, diags) {
  let pos = 0;

  const peek = () => tokens[pos];
  const check = (type) => tokens[pos].type === type;
  const advanceTok = () => tokens[pos++];
  const match = (type) => {
    if (check(type)) { advanceTok(); return true; }
    return false;
  };
  const expect = (type, what) => {
    if (check(type)) return advanceTok();
    const t = peek();
    diags.add('error', t.line, t.column, `Expected ${what} but found '${t.lexeme}'`);
    return null;
  };
  const skipToNextLine = () => {
    const l = peek().line;
    while (!check('TOKEN_EOF') && peek().line === l) advanceTok();
  };
  const skipStatement = () => {
    // panic recovery: skip to ';' or '}' without consuming the '}'
    while (!check('TOKEN_EOF') && !check('TOKEN_SEMICOLON') && !check('TOKEN_RBRACE')) advanceTok();
    match('TOKEN_SEMICOLON');
  };

  const TYPE_NAMES = { TOKEN_INT: 'int', TOKEN_FLOAT: 'float', TOKEN_DOUBLE: 'double', TOKEN_CHAR: 'char', TOKEN_VOID: 'void' };
  const isTypeToken = () => TYPE_TOKENS.has(peek().type);

  // ---- expressions ----
  function parseExpression() { return parseAssignment(); }

  function parseAssignment() {
    const cond = parseTernary();
    const t = peek();
    const ASSIGN_OPS = {
      TOKEN_ASSIGN: '=', TOKEN_PLUS_ASSIGN: '+=', TOKEN_MINUS_ASSIGN: '-=',
      TOKEN_STAR_ASSIGN: '*=', TOKEN_SLASH_ASSIGN: '/=', TOKEN_PERCENT_ASSIGN: '%=',
      TOKEN_AND_ASSIGN: '&=', TOKEN_OR_ASSIGN: '|=', TOKEN_XOR_ASSIGN: '^=',
      TOKEN_LSHIFT_ASSIGN: '<<=', TOKEN_RSHIFT_ASSIGN: '>>='
    };
    if (ASSIGN_OPS[t.type]) {
      advanceTok();
      const right = parseAssignment(); // right-associative
      const node = makeNode(ASSIGN_OPS[t.type] === '=' ? 'NODE_ASSIGNMENT' : 'NODE_COMPOUND_ASSIGN', t.line);
      node.op = ASSIGN_OPS[t.type];
      node.children.push(cond, right);
      return node;
    }
    return cond;
  }

  function parseTernary() {
    const cond = parseLogicalOr();
    if (peek().type === 'TOKEN_QUESTION') {
      const t = advanceTok();
      const node = makeNode('NODE_TERNARY', t.line);
      node.children.push(cond);
      node.children.push(parseAssignment());
      expect('TOKEN_COLON', "':' in ternary expression");
      node.children.push(parseTernary());
      return node;
    }
    return cond;
  }

  function binaryLevel(next, ops) {
    let left = next();
    while (ops[peek().type]) {
      const t = advanceTok();
      const node = makeNode('NODE_BINARY_OP', t.line);
      node.op = ops[t.type];
      node.children.push(left, next());
      left = node;
    }
    return left;
  }

  // C precedence: || < && < | < ^ < & < == < relational < shifts < additive < multiplicative
  const parseLogicalOr = () => binaryLevel(parseLogicalAnd, { TOKEN_OR: '||' });
  const parseLogicalAnd = () => binaryLevel(parseBitOr, { TOKEN_AND: '&&' });
  const parseBitOr = () => binaryLevel(parseBitXor, { TOKEN_BIT_OR: '|' });
  const parseBitXor = () => binaryLevel(parseBitAnd, { TOKEN_BIT_XOR: '^' });
  const parseBitAnd = () => binaryLevel(parseEquality, { TOKEN_AMPERSAND: '&' });
  const parseEquality = () => binaryLevel(parseRelational, { TOKEN_EQ: '==', TOKEN_NEQ: '!=' });
  const parseRelational = () => binaryLevel(parseShift, { TOKEN_LT: '<', TOKEN_GT: '>', TOKEN_LEQ: '<=', TOKEN_GEQ: '>=' });
  const parseShift = () => binaryLevel(parseAdditive, { TOKEN_LSHIFT: '<<', TOKEN_RSHIFT: '>>' });
  const parseAdditive = () => binaryLevel(parseMultiplicative, { TOKEN_PLUS: '+', TOKEN_MINUS: '-' });
  const parseMultiplicative = () => binaryLevel(parseUnary, { TOKEN_STAR: '*', TOKEN_SLASH: '/', TOKEN_PERCENT: '%' });

  const CAST_STARTERS = new Set([
    'TOKEN_INT', 'TOKEN_FLOAT', 'TOKEN_DOUBLE', 'TOKEN_CHAR', 'TOKEN_VOID',
    'TOKEN_SHORT', 'TOKEN_LONG', 'TOKEN_UNSIGNED', 'TOKEN_SIGNED',
    'TOKEN_CONST', 'TOKEN_VOLATILE', 'TOKEN_STRUCT'
  ]);
  const isTypeAhead = () => pos + 1 < tokens.length && CAST_STARTERS.has(tokens[pos + 1].type);

  // Parses a type specifier: storage/qualifier keywords, length modifiers,
  // base type or struct, plus pointer stars. Returns a normalized spec.
  function parseTypeSpec() {
    let isStatic = false, isExtern = false;
    let baseType = null, baseTok = null;
    let sawLong = 0, sawShort = false, sawUnsigned = false, sawSigned = false;

    for (;;) {
      const t = peek();
      if (t.type === 'TOKEN_STATIC') { advanceTok(); isStatic = true; continue; }
      if (t.type === 'TOKEN_EXTERN' || t.type === 'TOKEN_REGISTER') { advanceTok(); isExtern = true; continue; }
      if (t.type === 'TOKEN_CONST' || t.type === 'TOKEN_VOLATILE') { advanceTok(); continue; }
      if (t.type === 'TOKEN_IDENTIFIER' && t.lexeme === 'inline') { advanceTok(); continue; }
      if (t.type === 'TOKEN_SIGNED') { advanceTok(); sawSigned = true; continue; }
      if (t.type === 'TOKEN_UNSIGNED') { advanceTok(); sawUnsigned = true; continue; }
      if (t.type === 'TOKEN_SHORT') { advanceTok(); sawShort = true; continue; }
      if (t.type === 'TOKEN_LONG') { advanceTok(); sawLong++; continue; }
      if (t.type === 'TOKEN_INT' || t.type === 'TOKEN_FLOAT' || t.type === 'TOKEN_DOUBLE' ||
          t.type === 'TOKEN_CHAR' || t.type === 'TOKEN_VOID') {
        baseTok = advanceTok();
        baseType = { TOKEN_INT: 'int', TOKEN_FLOAT: 'float', TOKEN_DOUBLE: 'double',
                     TOKEN_CHAR: 'char', TOKEN_VOID: 'void' }[baseTok.type];
        continue; // allow `long long int`, `unsigned int`, ...
      }
      if (t.type === 'TOKEN_STRUCT' && !baseType) {
        advanceTok();
        const nameTok = expect('TOKEN_IDENTIFIER', 'struct name');
        baseType = 'struct ' + (nameTok ? nameTok.lexeme : '<error>');
        break;
      }
      break;
    }

    if (!baseType && (sawUnsigned || sawSigned || sawShort || sawLong)) baseType = 'int';

    // normalize to the VM's two value kinds (int / double), keep 'void'
    let normalized;
    if (baseType === 'float' || baseType === 'double') normalized = 'double';
    else if (baseType === 'void') normalized = 'void';
    else if (baseType && baseType.startsWith('struct ')) normalized = baseType;
    else normalized = 'int';

    let ptr = 0;
    while (check('TOKEN_STAR')) { advanceTok(); ptr++; }
    let typeName = normalized + '*'.repeat(ptr);

    return {
      typeName, normalized, ptr, isStatic, isExtern,
      baseKind: baseType || 'int',
      sawLong, sawShort, sawUnsigned, sawSigned,
      hasBase: !!baseType
    };
  }

  function sizeOfTypeSpec(spec) {
    if (spec.ptr > 0) return 8;
    switch (spec.baseKind) {
      case 'char': return 1;
      case 'short': return 2;
      case 'long': return spec.sawLong >= 2 ? 8 : 8;
      case 'float': return 4;
      case 'double': return 8;
      case 'void': return 1;
      default:
        if (spec.baseKind && spec.baseKind.startsWith('struct ')) return 4;
        return 4; /* int */
    }
  }

  // true if the current token starts a declaration type specifier
  // (storage class, qualifier, or length modifier — things isTypeToken misses)
  function isDeclTypeStart() {
    const t = peek();
    if (t.type === 'TOKEN_STATIC' || t.type === 'TOKEN_EXTERN' || t.type === 'TOKEN_REGISTER' ||
        t.type === 'TOKEN_CONST' || t.type === 'TOKEN_VOLATILE' ||
        t.type === 'TOKEN_UNSIGNED' || t.type === 'TOKEN_SIGNED' ||
        t.type === 'TOKEN_SHORT' || t.type === 'TOKEN_LONG') return true;
    if (t.type === 'TOKEN_IDENTIFIER' && t.lexeme === 'inline') return true;
    return false;
  }

  // Parse a local declaration whose type specifier was already consumed.
  // Detects and cleanly rejects nested function definitions & function pointers.
  function parseLocalDeclFromSpec(spec) {
    const startTok = peek();
    // Nested function definition:  int add(int a, int b) { ... }
    if (check('TOKEN_IDENTIFIER') && tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_LPAREN') {
      const afterParen = tokens[pos + 2];
      // heuristic: a '(' followed by a type / ')' looks like a parameter list
      if (afterParen && (CAST_STARTERS.has(afterParen.type) || afterParen.type === 'TOKEN_RPAREN' ||
                          afterParen.type === 'TOKEN_ELLIPSIS')) {
        diags.add('error', startTok.line, startTok.column,
          'Nested function definitions are not supported in the NOVA C subset (they are a GNU C extension)');
        skipDeclaration();
        return makeNode('NODE_EMPTY', startTok.line);
      }
    }
    // Function-pointer declarator:  int (*name)(...)
    if (check('TOKEN_LPAREN') && tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_STAR') {
      diags.add('error', peek().line, peek().column,
        'Function pointers are not supported in the NOVA C subset');
      skipDeclaration();
      return makeNode('NODE_EMPTY', startTok.line);
    }

    const line = startTok.line;
    const first = parseSingleDeclarator(spec.typeName, line, null);
    if (spec.isStatic) first.is_static = true;
    if (!check('TOKEN_COMMA')) {
      expect('TOKEN_SEMICOLON', "';' after declaration");
      return first;
    }
    const group = makeNode('NODE_DECL_LIST', line);
    group.children.push(first);
    while (match('TOKEN_COMMA')) {
      const d = parseSingleDeclarator(spec.typeName, line, null);
      if (spec.isStatic) d.is_static = true;
      group.children.push(d);
    }
    expect('TOKEN_SEMICOLON', "';' after declaration");
    return group;
  }

  function parseCast() {
    const t = advanceTok(); // '('
    const spec = parseTypeSpec();
    expect('TOKEN_RPAREN', "')' after cast type");
    const node = makeNode('NODE_CAST', t.line);
    node.type_name = spec.typeName;
    node.children.push(parseUnary());
    return node;
  }

  function parseSizeof() {
    const t = advanceTok(); // 'sizeof'
    if (check('TOKEN_LPAREN') && isTypeAhead()) {
      advanceTok(); // '('
      const spec = parseTypeSpec();
      expect('TOKEN_RPAREN', "')' after sizeof type");
      const lit = makeNode('NODE_INT_LITERAL', t.line);
      lit.num_val = sizeOfTypeSpec(spec);
      return lit;
    }
    const node = makeNode('NODE_SIZEOF', t.line);
    node.children.push(parseUnary());
    return node;
  }

  function parseUnary() {
    const t = peek();
    if (t.type === 'TOKEN_MINUS' || t.type === 'TOKEN_NOT' || t.type === 'TOKEN_STAR' ||
        t.type === 'TOKEN_AMPERSAND' || t.type === 'TOKEN_PLUS_PLUS' || t.type === 'TOKEN_MINUS_MINUS' ||
        t.type === 'TOKEN_BIT_NOT') {
      advanceTok();
      const node = makeNode('NODE_UNARY_OP', t.line);
      node.op = { TOKEN_MINUS: '-', TOKEN_NOT: '!', TOKEN_STAR: '*', TOKEN_AMPERSAND: '&',
                  TOKEN_PLUS_PLUS: '++', TOKEN_MINUS_MINUS: '--', TOKEN_BIT_NOT: '~' }[t.type];
      node.children.push(parseUnary());
      return node;
    }
    if (t.type === 'TOKEN_SIZEOF') return parseSizeof();
    if (t.type === 'TOKEN_LPAREN' && isTypeAhead()) return parseCast();
    return parsePostfix();
  }

  function parsePostfix() {
    let expr = parsePrimary();
    for (;;) {
      const t = peek();
      if (t.type === 'TOKEN_LPAREN' && expr.type === 'NODE_IDENTIFIER' && expr.identifier === 'offsetof') {
        // offsetof(type, member) is a libc macro we cannot evaluate — skip its
        // balanced argument parentheses with one clear diagnostic.
        diags.add('error', expr.line, 1, "'offsetof' is not supported in the NOVA C subset");
        advanceTok(); // '('
        let depth = 1;
        while (!check('TOKEN_EOF') && depth > 0) {
          if (check('TOKEN_LPAREN')) depth++;
          else if (check('TOKEN_RPAREN')) depth--;
          advanceTok();
        }
        const lit = makeNode('NODE_INT_LITERAL', expr.line);
        lit.num_val = 0;
        expr = lit;
        continue;
      }
      if (t.type === 'TOKEN_LPAREN') {
        advanceTok();
        const call = makeNode('NODE_FUNC_CALL', t.line);
        call.identifier = expr.type === 'NODE_IDENTIFIER' ? expr.identifier : '<expr>';
        if (!check('TOKEN_RPAREN')) {
          for (;;) {
            call.children.push(parseExpression());
            if (match('TOKEN_COMMA')) continue;
            break;
          }
        }
        expect('TOKEN_RPAREN', "')' after arguments");
        expr = call;
      } else if (t.type === 'TOKEN_LBRACKET') {
        advanceTok();
        const idx = makeNode('NODE_INDEX', t.line);
        idx.children.push(expr, parseExpression());
        expect('TOKEN_RBRACKET', "']' after index");
        expr = idx;
      } else if (t.type === 'TOKEN_DOT' || t.type === 'TOKEN_ARROW') {
        advanceTok();
        const mem = makeNode('NODE_MEMBER', t.line);
        mem.children.push(expr);
        const id = expect('TOKEN_IDENTIFIER', 'field name after "."');
        mem.identifier = id ? id.lexeme : '<error>';
        expr = mem;
      } else if (t.type === 'TOKEN_PLUS_PLUS' || t.type === 'TOKEN_MINUS_MINUS') {
        advanceTok();
        const node = makeNode('NODE_UNARY_OP', t.line);
        node.op = t.type === 'TOKEN_PLUS_PLUS' ? 'p++' : 'p--';
        node.children.push(expr);
        expr = node;
      } else {
        break;
      }
    }
    return expr;
  }

  function parsePrimary() {
    const t = peek();
    if (t.type === 'TOKEN_INTEGER_LITERAL') {
      advanceTok();
      const node = makeNode('NODE_INT_LITERAL', t.line);
      node.num_val = parseInt(t.lexeme, t.lexeme.startsWith('0x') || t.lexeme.startsWith('0X') ? 16 : 10);
      return node;
    }
    if (t.type === 'TOKEN_FLOAT_LITERAL') {
      advanceTok();
      const node = makeNode('NODE_FLOAT_LITERAL', t.line);
      node.num_val = parseFloat(t.lexeme);
      return node;
    }
    if (t.type === 'TOKEN_CHAR_LITERAL') {
      advanceTok();
      const node = makeNode('NODE_INT_LITERAL', t.line);
      node.num_val = t.charValue;
      return node;
    }
    if (t.type === 'TOKEN_STRING_LITERAL') {
      const first = advanceTok();
      let value = first.stringValue;
      let lexeme = first.lexeme;
      // C translation phase 6: adjacent string literal concatenation
      while (check('TOKEN_STRING_LITERAL')) {
        const nxt = advanceTok();
        value += nxt.stringValue;
        lexeme = lexeme.slice(0, -1) + nxt.lexeme.slice(1);
      }
      const node = makeNode('NODE_STRING_LITERAL', t.line);
      node.string_val = value;
      return node;
    }
    if (t.type === 'TOKEN_IDENTIFIER') {
      advanceTok();
      const node = makeNode('NODE_IDENTIFIER', t.line);
      node.identifier = t.lexeme;
      return node;
    }
    if (t.type === 'TOKEN_LPAREN') {
      advanceTok();
      const inner = parseExpression();
      expect('TOKEN_RPAREN', "')'");
      return inner;
    }
    diags.add('error', t.line, t.column, `Unexpected token '${t.lexeme}' in expression`);
    advanceTok();
    const node = makeNode('NODE_ERROR', t.line);
    return node;
  }

  // ---- declarations & statements ----

  function parseSingleDeclarator(typeName, line, firstIdTok) {
    // declarator: ['*'] identifier ( '[' size ']' )? ( '=' init )?
    const decl = makeNode('NODE_VAR_DECL', line);
    let type = typeName;
    if (!firstIdTok) {
      while (check('TOKEN_STAR')) { advanceTok(); type += '*'; }
    }
    const id = firstIdTok || expect('TOKEN_IDENTIFIER', 'variable name');
    decl.identifier = id ? id.lexeme : '<error>';
    decl.type_name = type;
    if (match('TOKEN_LBRACKET')) {
      decl.is_array = true;
      if (check('TOKEN_INTEGER_LITERAL')) {
        const sz = advanceTok();
        decl.children.push((() => { const nn = makeNode('NODE_INT_LITERAL', sz.line); nn.num_val = parseInt(sz.lexeme, 10); return nn; })());
        decl.has_size = true;
      }
      expect('TOKEN_RBRACKET', "']' after array size");
    }
    if (match('TOKEN_ASSIGN')) {
      if ((decl.is_array || type.startsWith('struct ')) && check('TOKEN_LBRACE')) {
        advanceTok();
        while (!check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
          decl.children.push(parseExpression());
          if (match('TOKEN_COMMA')) continue;
          break;
        }
        expect('TOKEN_RBRACE', "'}' after initializer list");
      } else {
        decl.children.push(parseExpression());
      }
    }
    return decl;
  }

  function parseVarDeclTail(typeName, line) {
    // one or more comma-separated declarators followed by ';'
    const first = parseSingleDeclarator(typeName, line, null);
    if (!check('TOKEN_COMMA')) {
      expect('TOKEN_SEMICOLON', "';' after declaration");
      return first;
    }
    const group = makeNode('NODE_DECL_LIST', line);
    group.children.push(first);
    while (match('TOKEN_COMMA')) {
      group.children.push(parseSingleDeclarator(typeName, line, null));
    }
    expect('TOKEN_SEMICOLON', "';' after declaration");
    return group;
  }

  // Skip tokens until ';' at nesting depth 0 (consuming it), or until a
  // top-level '}' — used for graceful recovery on unsupported declarations.
  function skipDeclaration() {
    let depth = 0;
    while (!check('TOKEN_EOF')) {
      const ty = peek().type;
      if (ty === 'TOKEN_LPAREN' || ty === 'TOKEN_LBRACKET' || ty === 'TOKEN_LBRACE') { depth++; advanceTok(); continue; }
      if (ty === 'TOKEN_RPAREN' || ty === 'TOKEN_RBRACKET') { if (depth > 0) depth--; advanceTok(); continue; }
      if (ty === 'TOKEN_RBRACE') {
        if (depth === 0) return; // leave the brace for the caller
        depth--; advanceTok(); continue;
      }
      if (ty === 'TOKEN_SEMICOLON' && depth === 0) { advanceTok(); return; }
      advanceTok();
    }
  }

  function parseStatement(inLoop, inSwitch) {
    let t = peek();

    // preprocessor directives may appear inside bodies; skip the line
    if (t.type === 'TOKEN_INCLUDE' || t.type === 'TOKEN_DEFINE' || t.type === 'TOKEN_HASH') {
      const l = t.line;
      while (!check('TOKEN_EOF') && peek().line === l) advanceTok();
      return makeNode('NODE_EMPTY', t.line);
    }

    if (t.type === 'TOKEN_IF') {
      advanceTok();
      const node = makeNode('NODE_IF_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'if'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after condition");
      node.children.push(parseStatement(inLoop, inSwitch));
      if (match('TOKEN_ELSE')) node.children.push(parseStatement(inLoop, inSwitch));
      return node;
    }
    if (t.type === 'TOKEN_WHILE') {
      advanceTok();
      const node = makeNode('NODE_WHILE_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'while'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after condition");
      node.children.push(parseStatement(true, inSwitch));
      return node;
    }
    if (t.type === 'TOKEN_DO') {
      advanceTok();
      const node = makeNode('NODE_DO_WHILE_STMT', t.line);
      node.children.push(parseStatement(true, inSwitch));
      expect('TOKEN_WHILE', "'while' after do-body");
      expect('TOKEN_LPAREN', "'(' after 'while'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after do-while condition");
      expect('TOKEN_SEMICOLON', "';' after do-while");
      return node;
    }
    if (t.type === 'TOKEN_SWITCH') {
      advanceTok();
      const node = makeNode('NODE_SWITCH_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'switch'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after switch expression");
      expect('TOKEN_LBRACE', "'{' after switch expression");
      while (!check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
        if (check('TOKEN_CASE')) {
          const ct = advanceTok();
          const caseNode = makeNode('NODE_CASE', ct.line);
          let sign = 1;
          if (check('TOKEN_MINUS')) { advanceTok(); sign = -1; }
          if (check('TOKEN_INTEGER_LITERAL') || check('TOKEN_CHAR_LITERAL')) {
            const v = advanceTok();
            caseNode.num_val = sign * (v.type === 'TOKEN_CHAR_LITERAL' ? v.charValue : parseInt(v.lexeme, 0));
          } else {
            diagCaseError(ct);
          }
          expect('TOKEN_COLON', "':' after case value");
          while (!check('TOKEN_CASE') && !check('TOKEN_DEFAULT') &&
                 !check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
            caseNode.children.push(parseStatement(inLoop, true));
          }
          node.children.push(caseNode);
        } else if (check('TOKEN_DEFAULT')) {
          const dt = advanceTok();
          const defNode = makeNode('NODE_DEFAULT', dt.line);
          expect('TOKEN_COLON', "':' after 'default'");
          while (!check('TOKEN_CASE') && !check('TOKEN_DEFAULT') &&
                 !check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
            defNode.children.push(parseStatement(inLoop, true));
          }
          node.children.push(defNode);
        } else {
          const tok = peek();
          diags.add('error', tok.line, tok.column, `Expected 'case' or 'default' in switch but found '${tok.lexeme}'`);
          advanceTok();
        }
      }
      expect('TOKEN_RBRACE', "'}' to close switch");
      return node;
    }
    function diagCaseError(ct) {
      diags.add('error', ct.line, ct.column, 'Case value must be an integer constant in the NOVA subset');
    }
    if (t.type === 'TOKEN_GOTO') {
      advanceTok();
      const node = makeNode('NODE_GOTO', t.line);
      const id = expect('TOKEN_IDENTIFIER', 'label name after goto');
      node.identifier = id ? id.lexeme : '<error>';
      node.has_identifier = true;
      expect('TOKEN_SEMICOLON', "';' after goto");
      return node;
    }
    if (t.type === 'TOKEN_FOR') {
      advanceTok();
      const node = makeNode('NODE_FOR_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'for'");
      // init
      if (check('TOKEN_SEMICOLON')) {
        advanceTok();
        node.children.push(makeNode('NODE_EMPTY', t.line));
      } else if (isTypeToken() || isDeclTypeStart()) {
        const spec = parseTypeSpec();
        node.children.push(parseVarDeclTail(spec.typeName, t.line));
      } else {
        const e = makeNode('NODE_EXPRESSION_STMT', t.line);
        e.children.push(parseExpression());
        expect('TOKEN_SEMICOLON', "';' after for-loop initializer");
        node.children.push(e);
      }
      // condition
      if (check('TOKEN_SEMICOLON')) {
        node.children.push(makeNode('NODE_EMPTY', t.line));
      } else {
        node.children.push(parseExpression());
      }
      expect('TOKEN_SEMICOLON', "';' after for-loop condition");
      // increment
      if (check('TOKEN_RPAREN')) {
        node.children.push(makeNode('NODE_EMPTY', t.line));
      } else {
        node.children.push(parseExpression());
      }
      expect('TOKEN_RPAREN', "')' after for-loop clauses");
      node.children.push(parseStatement(true, inSwitch));
      return node;
    }
    if (t.type === 'TOKEN_RETURN') {
      advanceTok();
      const node = makeNode('NODE_RETURN_STMT', t.line);
      if (!check('TOKEN_SEMICOLON')) node.children.push(parseExpression());
      expect('TOKEN_SEMICOLON', "';' after return");
      return node;
    }
    if (t.type === 'TOKEN_BREAK') {
      advanceTok();
      expect('TOKEN_SEMICOLON', "';' after break");
      return makeNode('NODE_BREAK_STMT', t.line);
    }
    if (t.type === 'TOKEN_CONTINUE') {
      advanceTok();
      expect('TOKEN_SEMICOLON', "';' after continue");
      return makeNode('NODE_CONTINUE_STMT', t.line);
    }
    if (t.type === 'TOKEN_LBRACE') {
      advanceTok();
      const comp = makeNode('NODE_COMPOUND_STMT', t.line);
      while (!check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
        comp.children.push(parseStatement(inLoop, inSwitch));
      }
      expect('TOKEN_RBRACE', "'}' to close block");
      return comp;
    }
    // Label:  name: statement
    if (t.type === 'TOKEN_IDENTIFIER' && tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_COLON') {
      advanceTok();
      const labelNode = makeNode('NODE_LABEL_STMT', t.line);
      labelNode.identifier = t.lexeme;
      advanceTok(); // ':'
      labelNode.children.push(parseStatement(inLoop, inSwitch));
      return labelNode;
    }
    // Local declaration starting with any type specifier / qualifier
    if (isTypeToken() || isDeclTypeStart()) {
      const spec = parseTypeSpec();
      if (!spec.hasBase && !(spec.sawUnsigned || spec.sawSigned || spec.sawShort || spec.sawLong)) {
        // only qualifiers seen (e.g. a stray 'const') — fall through to expr
      } else if (check('TOKEN_IDENTIFIER') && tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_IDENTIFIER') {
        // multi-word types we don't support: 'double complex z', etc.
        diags.add('error', t.line, t.column,
          `Unsupported type combination near '${peek().lexeme}' (e.g. complex numbers are not in the NOVA subset)`);
        skipDeclaration();
        return makeNode('NODE_EMPTY', t.line);
      } else {
        return parseLocalDeclFromSpec(spec);
      }
    }
    if (t.type === 'TOKEN_UNION' || t.type === 'TOKEN_ENUM') {
      diags.add('error', t.line, t.column, `'${t.lexeme}' is not supported in the NOVA C subset`);
      skipDeclaration();
      return makeNode('NODE_EMPTY', t.line);
    }
    // Unknown (typedef'd) type in a local declaration:  FILE *f;  va_list args;
    if (t.type === 'TOKEN_IDENTIFIER' && tokens[pos + 1] &&
        (tokens[pos + 1].type === 'TOKEN_IDENTIFIER' || tokens[pos + 1].type === 'TOKEN_STAR')) {
      diags.add('error', t.line, t.column,
        `Unknown type '${t.lexeme}' — typedef names are not supported in the NOVA C subset`);
      skipDeclaration();
      return makeNode('NODE_EMPTY', t.line);
    }
    if (t.type === 'TOKEN_STRUCT') {
      // local struct definition (unsupported) or struct-typed variable
      if (tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_IDENTIFIER' &&
          tokens[pos + 2] && tokens[pos + 2].type === 'TOKEN_LBRACE') {
        diags.add('error', t.line, t.column,
          'Local struct definitions are not supported in the NOVA C subset');
        skipDeclaration();
        return makeNode('NODE_EMPTY', t.line);
      }
      advanceTok();
      const nameTok = expect('TOKEN_IDENTIFIER', 'struct name');
      return parseVarDeclTail(`struct ${nameTok ? nameTok.lexeme : '<error>'}`, t.line);
    }
    if (t.type === 'TOKEN_SEMICOLON') {
      advanceTok();
      return makeNode('NODE_EMPTY', t.line);
    }

    // expression statement
    const before = pos;
    const expr = parseExpression();
    if (pos === before) {
      diags.add('error', t.line, t.column, `Unexpected token '${t.lexeme}'`);
      advanceTok();
      return makeNode('NODE_ERROR', t.line);
    }
    const stmt = makeNode('NODE_EXPRESSION_STMT', t.line);
    stmt.children.push(expr);
    expect('TOKEN_SEMICOLON', "';' after expression");
    return stmt;
  }

  function parseParameterList(func) {
    if (check('TOKEN_RPAREN')) return;
    if (check('TOKEN_VOID') && tokens[pos + 1].type === 'TOKEN_RPAREN') {
      advanceTok(); // `int main(void)`
      return;
    }
    for (;;) {
      if (check('TOKEN_ELLIPSIS')) {
        const et = advanceTok();
        diags.add('error', et.line, et.column,
          'Variadic functions (...) are not supported in the NOVA C subset');
        continue;
      }
      if (!isTypeToken() && !isDeclTypeStart()) {
        diags.add('error', peek().line, peek().column, `Expected parameter type but found '${peek().lexeme}'`);
        break;
      }
      const spec = parseTypeSpec();
      const id = expect('TOKEN_IDENTIFIER', 'parameter name');
      const param = makeNode('NODE_PARAMETER', id ? id.line : func.line);
      param.type_name = spec.typeName;
      param.identifier = id ? id.lexeme : '<error>';
      // array parameter: int arr[] (optional size accepted and ignored —
      // the parameter decays to a pointer, exactly like C)
      if (match('TOKEN_LBRACKET')) {
        while (!check('TOKEN_RBRACKET') && !check('TOKEN_EOF') && !check('TOKEN_COMMA')) advanceTok();
        if (match('TOKEN_RBRACKET')) { /* consumed */ }
        param.is_array = true;
      }
      func.children.push(param);
      if (match('TOKEN_COMMA')) continue;
      break;
    }
  }

  // ---- top level ----
  const root = makeNode('NODE_PROGRAM', 1);

  while (!check('TOKEN_EOF')) {
    const t = peek();

    if (t.type === 'TOKEN_INCLUDE' || t.type === 'TOKEN_DEFINE' || t.type === 'TOKEN_HASH') {
      skipToNextLine();
      continue;
    }

    if (t.type === 'TOKEN_TYPEDEF') {
      advanceTok();
      diags.add('error', t.line, t.column, 'typedef is not supported in the NOVA C subset');
      skipDeclaration();
      continue;
    }
    if (t.type === 'TOKEN_UNION' || t.type === 'TOKEN_ENUM') {
      diags.add('error', t.line, t.column, `'${t.lexeme}' is not supported in the NOVA C subset`);
      skipDeclaration();
      continue;
    }

    if (t.type === 'TOKEN_STRUCT') {
      advanceTok();
      const nameTok = expect('TOKEN_IDENTIFIER', 'struct name');
      if (check('TOKEN_LBRACE')) {
        advanceTok();
        const def = makeNode('NODE_STRUCT_DEF', t.line);
        def.identifier = nameTok ? nameTok.lexeme : '<error>';
        while (!check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
          const ft = peek();
          if (ft.type === 'TOKEN_UNION' || ft.type === 'TOKEN_UNSIGNED' ||
              ft.type === 'TOKEN_SIGNED' || ft.type === 'TOKEN_SHORT' ||
              ft.type === 'TOKEN_LONG') {
            diags.add('error', ft.line, ft.column,
              `'${ft.lexeme}' struct fields are not supported in the NOVA C subset`);
            skipDeclaration();
            continue;
          }
          if (!isTypeToken() && !isDeclTypeStart() && ft.type !== 'TOKEN_STRUCT') {
            diags.add('error', ft.line, ft.column, `Expected field type in struct but found '${ft.lexeme}'`);
            skipDeclaration();
            continue;
          }
          const fspec = parseTypeSpec();
          const field = makeNode('NODE_STRUCT_FIELD', ft.line);
          field.type_name = fspec.typeName;
          const fid = expect('TOKEN_IDENTIFIER', 'field name');
          field.identifier = fid ? fid.lexeme : '<error>';
          if (match('TOKEN_LBRACKET')) {
            if (!check('TOKEN_INTEGER_LITERAL')) {
              diags.add('error', peek().line, peek().column,
                'Flexible array members are not supported in the NOVA C subset');
              expect('TOKEN_RBRACKET', "']' after field size");
            } else {
              field.is_array = true;
              const sz = advanceTok();
              field.children.push((() => { const nn = makeNode('NODE_INT_LITERAL', sz.line); nn.num_val = parseInt(sz.lexeme, 10); return nn; })());
              field.has_size = true;
              expect('TOKEN_RBRACKET', "']' after field size");
            }
          }
          if (check('TOKEN_COLON')) {
            advanceTok();
            diags.add('error', ft.line, ft.column, 'Bitfields are not supported in the NOVA C subset');
            if (check('TOKEN_INTEGER_LITERAL')) advanceTok();
          }
          expect('TOKEN_SEMICOLON', "';' after struct field");
          def.children.push(field);
        }
        expect('TOKEN_RBRACE', "'}' to close struct");
        expect('TOKEN_SEMICOLON', "';' after struct definition");
        root.children.push(def);
      } else {
        // global struct variable
        root.children.push(parseVarDeclTail(`struct ${nameTok ? nameTok.lexeme : '<error>'}`, t.line));
      }
      continue;
    }

    if (isTypeToken() || isDeclTypeStart()) {
      const spec = parseTypeSpec();
      if (!spec.hasBase && !(spec.sawUnsigned || spec.sawSigned || spec.sawShort || spec.sawLong)) {
        diags.add('error', peek().line, peek().column, `Expected a type in declaration near '${peek().lexeme}'`);
        skipDeclaration();
        continue;
      }
      // function-pointer global:  int (*name)(...)
      if (check('TOKEN_LPAREN') && tokens[pos + 1] && tokens[pos + 1].type === 'TOKEN_STAR') {
        diags.add('error', peek().line, peek().column,
          'Function pointers are not supported in the NOVA C subset');
        skipDeclaration();
        continue;
      }
      const id = expect('TOKEN_IDENTIFIER', 'function or variable name');
      if (!id) { skipDeclaration(); continue; }
      if (check('TOKEN_LPAREN')) {
        advanceTok();
        const func = makeNode('NODE_FUNCTION_DEF', t.line);
        func.type_name = spec.typeName;
        func.identifier = id.lexeme;
        parseParameterList(func);
        expect('TOKEN_RPAREN', "')' after parameters");
        if (match('TOKEN_SEMICOLON')) {
          func.is_forward = true; // forward declaration, no body
          root.children.push(func);
          continue;
        }
        if (!check('TOKEN_LBRACE')) {
          diags.add('error', peek().line, peek().column, "Expected '{' after function signature");
          skipDeclaration();
          root.children.push(func);
          continue;
        }
        func.children.push(parseStatement(false, false)); // body
        root.children.push(func);
      } else {
        // global variable(s), possibly comma-separated
        const first = parseSingleDeclarator(spec.typeName, t.line, id);
        if (spec.isStatic) first.is_static = true;
        if (!check('TOKEN_COMMA')) {
          expect('TOKEN_SEMICOLON', "';' after declaration");
          root.children.push(first);
        } else {
          const group = makeNode('NODE_DECL_LIST', t.line);
          group.children.push(first);
          while (match('TOKEN_COMMA')) {
            const d = parseSingleDeclarator(spec.typeName, t.line, null);
            if (spec.isStatic) d.is_static = true;
            group.children.push(d);
          }
          expect('TOKEN_SEMICOLON', "';' after declaration");
          root.children.push(group);
        }
      }
      continue;
    }

    // Declaration with an unknown (typedef'd) type:  jmp_buf env;  FILE *f;
    if (t.type === 'TOKEN_IDENTIFIER' && tokens[pos + 1] &&
        (tokens[pos + 1].type === 'TOKEN_IDENTIFIER' || tokens[pos + 1].type === 'TOKEN_STAR')) {
      diags.add('error', t.line, t.column,
        `Unknown type '${t.lexeme}' — typedef names are not supported in the NOVA C subset`);
      skipDeclaration();
      continue;
    }

    diags.add('error', t.line, t.column, `Expected declaration but found '${t.lexeme}'`);
    advanceTok();
  }

  return root;
}

// ---------------------------------------------------------------------------
// Phase 3: Semantic analysis + memory layout
// ---------------------------------------------------------------------------

// Built-in functions callable from NOVA programs (VM-implemented).
// arity -1 = variadic (printf/scanf). Deterministic and identical in both engines.
const BUILTIN_FUNCTIONS = [
  { name: 'printf', type: 'int', params: -1 },
  { name: 'scanf', type: 'int', params: -1 },
  { name: 'abs', type: 'int', params: 1 },
  { name: 'assert', type: 'int', params: 1 },
  { name: 'ceil', type: 'double', params: 1 },
  { name: 'cos', type: 'double', params: 1 },
  { name: 'exit', type: 'int', params: 1 },
  { name: 'exp', type: 'double', params: 1 },
  { name: 'fabs', type: 'double', params: 1 },
  { name: 'floor', type: 'double', params: 1 },
  { name: 'fmod', type: 'double', params: 2 },
  { name: 'log', type: 'double', params: 1 },
  { name: 'pow', type: 'double', params: 2 },
  { name: 'rand', type: 'int', params: 0 },
  { name: 'sin', type: 'double', params: 1 },
  { name: 'sqrt', type: 'double', params: 1 },
  { name: 'srand', type: 'int', params: 1 },
  { name: 'tan', type: 'double', params: 1 },
  { name: 'time', type: 'int', params: 1 }
];

// Functions that exist in libc but cannot run in the deterministic educational
// VM — calls produce a clear diagnostic instead of cascading errors.
const UNSUPPORTED_FUNCTIONS = new Set([
  'malloc', 'calloc', 'realloc', 'free',
  'fopen', 'fclose', 'fprintf', 'fscanf', 'fgets', 'fputs', 'fread', 'fwrite',
  'fseek', 'ftell', 'rewind', 'fflush', 'setbuf', 'setvbuf', 'tmpfile', 'tmpnam',
  'sprintf', 'snprintf', 'sscanf', 'vprintf', 'vfprintf',
  'memcpy', 'memset', 'memmove', 'memcmp',
  'strcmp', 'strcpy', 'strcat', 'strlen', 'strchr', 'strstr', 'strncpy',
  'strtol', 'strtod', 'atoi', 'atof',
  'setjmp', 'longjmp', 'signal', 'perror', 'remove', 'rename',
  'clock', 'va_start', 'va_arg', 'va_end', 'creal', 'cimag', 'qsort', 'bsearch'
]);

function analyzeSemantics(ast, diags) {
  const symbols = [];            // serialized symbol table (insertion order)
  const globals = new Map();     // name -> symbol record
  const functions = new Map();   // name -> {node, returnType, params:[{name,type}], frame:Map}
  const structs = new Map();     // name -> {fields:Map(name->{type,offset,size,is_array}), size}
  const staticDecls = [];        // static locals, promoted to global storage
  const globalSlots = { next: 0 };

  // built-ins first (stable order for the symbol table UI)
  for (const b of BUILTIN_FUNCTIONS) {
    symbols.push({ scope: 'global', name: b.name, kind: 'Function', type: b.type, address: '0x0000', params: b.params });
  }

  const baseTypeOf = (typeName) => typeName.replace(/\*/g, '').trim();
  const isPointerType = (typeName) => typeName.endsWith('*');
  const typeSize = (typeName) => {
    if (typeName.startsWith('struct ')) {
      const s = structs.get(typeName.slice(7));
      return s ? s.size : 1;
    }
    return 1;
  };

  // built-in global: errno (mutable integer, starts at 0)
  {
    const rec = {
      name: 'errno', storageName: 'errno', type: 'int', is_array: false, size: 1,
      isGlobal: true, offset: globalSlots.next, isTemp: false, isParam: false, isBuiltinVar: true
    };
    globalSlots.next += 1;
    globals.set('errno', rec);
    symbols.push({ scope: 'global', name: 'errno', kind: 'Variable', type: 'int', address: '0x0000', params: 0 });
  }

  // Pass 1: collect structs, globals, function signatures
  for (const node of ast.children) {
    if (node.type === 'NODE_STRUCT_DEF') {
      if (structs.has(node.identifier)) {
        diags.add('error', node.line, 1, `Redefinition of struct '${node.identifier}'`);
        continue;
      }
      const fields = new Map();
      let offset = 0;
      for (const f of node.children) {
        const size = f.is_array ? (f.has_size ? Math.max(1, truncateToInteger(f.children[0].num_val)) : 1)
          : (f.type_name.startsWith('struct ') ? (structs.get(f.type_name.slice(7)) || { size: 1 }).size : 1);
        fields.set(f.identifier, { type: f.type_name, offset, size, is_array: !!f.is_array });
        offset += size;
      }
      structs.set(node.identifier, { fields, size: offset });
      symbols.push({ scope: 'global', name: node.identifier, kind: 'Struct', type: 'struct', address: '0x0000', params: fields.size });
    }
  }

  for (const raw of ast.children) {
    const declNodes = raw.type === 'NODE_DECL_LIST' ? raw.children : [raw];
    for (const node of declNodes) {
    if (node.type === 'NODE_FUNCTION_DEF') {
      if (functions.has(node.identifier)) {
        const existing = functions.get(node.identifier);
        if (existing.node.is_forward && !node.is_forward) {
          // the real definition replaces the forward declaration
          functions.set(node.identifier, {
            node, returnType: node.type_name,
            params: node.children.filter((c) => c.type === 'NODE_PARAMETER')
              .map((p) => ({ name: p.identifier, type: p.type_name, is_array: !!p.is_array })),
            frame: new Map(), storage: new Map(), frameSize: 0
          });
          continue;
        }
        diags.add('error', node.line, 1, `Redefinition of function '${node.identifier}'`);
        continue;
      }
      const params = node.children.filter((c) => c.type === 'NODE_PARAMETER')
        .map((p) => ({ name: p.identifier, type: p.type_name, is_array: !!p.is_array }));
      functions.set(node.identifier, { node, returnType: node.type_name, params, frame: new Map(), storage: new Map(), frameSize: 0 });
      symbols.push({
        scope: 'global', name: node.identifier, kind: 'Function', type: node.type_name,
        address: '0x0000', params: params.length
      });
    } else if (node.type === 'NODE_VAR_DECL') {
      if (globals.has(node.identifier)) {
        diags.add('error', node.line, 1, `Redefinition of global '${node.identifier}'`);
        continue;
      }
      const size = node.is_array
        ? (node.has_size ? Math.max(1, truncateToInteger(node.children[0].num_val))
           : (node.children.length > 1 ? node.children.length
              : (node.children.length === 1 && node.children[0].type === 'NODE_STRING_LITERAL'
                 ? String(node.children[0].string_val).length + 1 : 1)))
        : typeSize(node.type_name);
      const rec = {
        name: node.identifier, storageName: node.identifier,
        type: node.type_name, is_array: !!node.is_array,
        size, isGlobal: true, offset: globalSlots.next, isTemp: false, isParam: false
      };
      Object.defineProperty(node, '_binding', { value: rec, configurable: true });
      globalSlots.next += size;
      globals.set(node.identifier, rec);
      symbols.push({
        scope: 'global', name: node.identifier, kind: node.is_array ? 'Array' : 'Variable',
        type: node.type_name, address: '0x' + (rec.offset * 4).toString(16).toUpperCase().padStart(4, '0'), params: 0
      });
    }
    }
  }

  if (!functions.has('main')) {
    diags.add('error', 1, 1, "No 'main' function defined");
  }

  // Pass 2: check each function body, assign frame slots
  for (const [fname, frec] of functions) {
    const scopeStack = [new Map()];
    let nextSlot = 0;
    const frame = frec.frame;
    const storage = frec.storage;
    const declaredNames = new Set();

    const declare = (name, type, node, isParam) => {
      const scope = scopeStack[scopeStack.length - 1];
      if (scope.has(name)) {
        diags.add('error', node.line, 1, `Duplicate declaration of '${name}'`);
        return;
      }
      const size = node.is_array
        ? (node.has_size ? Math.max(1, truncateToInteger(node.children[0].num_val))
           : (node.children.length > 1 ? node.children.length
              : (node.children.length === 1 && node.children[0].type === 'NODE_STRING_LITERAL'
                 ? String(node.children[0].string_val).length + 1 : 1)))
        : typeSize(type);

      // static locals are promoted to global storage (persistent across calls)
      if (node.is_static && !isParam) {
        if (globals.has(name)) {
          diags.add('error', node.line, 1, `Redefinition of global '${name}'`);
          return;
        }
        const grec = {
          name, storageName: name, type, is_array: !!node.is_array, size,
          isGlobal: true, offset: globalSlots.next, isTemp: false,
          isParam: false, isStaticLocal: true
        };
        globalSlots.next += size;
        globals.set(name, grec);
        scope.set(name, grec);
        storage.set(grec.storageName, grec);
        Object.defineProperty(node, '_binding', { value: grec, configurable: true });
        symbols.push({
          scope: fname, name, kind: node.is_array ? 'Array' : 'Variable (static)',
          type, address: '0x' + (grec.offset * 4).toString(16).toUpperCase().padStart(4, '0'), params: 0
        });
        staticDecls.push(node);
        return;
      }

      const storageName = (globals.has(name) || declaredNames.has(name)) ? `$L${nextSlot}` : name;
      const rec = {
        name, storageName, type, is_array: !!node.is_array, size,
        isGlobal: false, offset: nextSlot, isTemp: false, isParam
      };
      nextSlot += size;
      declaredNames.add(name);
      scope.set(name, rec);
      frame.set(name, rec);
      storage.set(storageName, rec);
      Object.defineProperty(node, '_binding', { value: rec, configurable: true });
      symbols.push({
        scope: fname, name, kind: isParam ? 'Parameter' : (node.is_array ? 'Array' : 'Variable'),
        type, address: '0x' + (rec.offset * 4).toString(16).toUpperCase().padStart(4, '0'), params: 0
      });
    };

    for (const p of frec.node.children.filter((c) => c.type === 'NODE_PARAMETER')) {
      declare(p.identifier, p.type_name, p, true);
    }

    const lookup = (name) => {
      for (let i = scopeStack.length - 1; i >= 0; i--) {
        if (scopeStack[i].has(name)) return scopeStack[i].get(name);
      }
      return null;
    };

    const isBuiltinFunc = (name) => BUILTIN_FUNCTIONS.some((b) => b.name === name);

    const resolve = (name, line) => {
      const rec = lookup(name);
      if (rec) return rec;
      if (globals.has(name)) return globals.get(name);
      if (functions.has(name)) return { name, isFunction: true };
      if (isBuiltinFunc(name)) return { name, isFunction: true, isBuiltin: true };
      diags.add('error', line, 1, `Undefined identifier '${name}'`);
      return { name, type: 'int', size: 1, isGlobal: false, offset: 0, isTemp: false, isPhantom: true };
    };

    const exprType = (node) => {
      if (!node) return 'int';
      switch (node.type) {
        case 'NODE_INT_LITERAL': return 'int';
        case 'NODE_FLOAT_LITERAL': return 'double';
        case 'NODE_STRING_LITERAL': return 'char*';
        case 'NODE_IDENTIFIER': {
          const rec = resolve(node.identifier, node.line);
          return rec.type || 'int';
        }
        case 'NODE_UNARY_OP': {
          if (node.op === '&') {
            const inner = node.children[0];
            if (inner && inner.type === 'NODE_IDENTIFIER') {
              const rec = resolve(inner.identifier, inner.line);
              if (rec.is_array) return (rec.type || 'int') + '*';
              return (rec.type || 'int') + '*';
            }
            return 'int*';
          }
          if (node.op === '*') {
            const t = exprType(node.children[0]);
            return isPointerType(t) ? t.slice(0, -1) : 'int';
          }
          if (node.op === '!') return 'int';
          if (node.op === '-' || node.op === '++' || node.op === '--' || node.op === 'p++' || node.op === 'p--') {
            return exprType(node.children[0]);
          }
          return 'int';
        }
        case 'NODE_BINARY_OP': {
          const op = node.op;
          if (['==', '!=', '<', '>', '<=', '>=', '&&', '||'].includes(op)) return 'int';
          const lt = exprType(node.children[0]);
          const rt = exprType(node.children[1]);
          const isF = (t) => t === 'float' || t === 'double';
          return (isF(lt) || isF(rt)) ? 'double' : 'int';
        }
        case 'NODE_INDEX': {
          const baseT = exprType(node.children[0]);
          return isPointerType(baseT) ? baseT.slice(0, -1) : baseT;
        }
        case 'NODE_MEMBER': {
          const base = node.children[0];
          if (base && base.type === 'NODE_IDENTIFIER') {
            const rec = resolve(base.identifier, base.line);
            const tname = rec.type || '';
            if (tname.startsWith('struct ')) {
              const s = structs.get(tname.slice(7));
              if (s) {
                const f = s.fields.get(node.identifier);
                if (f) return f.is_array ? f.type + '*' : f.type;
                diags.add('error', node.line, 1, `Struct '${tname.slice(7)}' has no field '${node.identifier}'`);
                return 'int';
              }
            }
          }
          diags.add('error', node.line, 1, `Member access on non-struct expression`);
          return 'int';
        }
        case 'NODE_FUNC_CALL': {
          const bf = BUILTIN_FUNCTIONS.find((b) => b.name === node.identifier);
          if (bf) {
            if (bf.params >= 0 && node.children.length !== bf.params) {
              diags.add('error', node.line, 1,
                `Function '${node.identifier}' expects ${bf.params} argument(s), got ${node.children.length}`);
            }
            return bf.type;
          }
          const f = functions.get(node.identifier);
          if (!f) {
            if (UNSUPPORTED_FUNCTIONS.has(node.identifier)) {
              diags.add('error', node.line, 1,
                `Function '${node.identifier}' is not supported in the NOVA C subset (no libc/heap/IO in the educational VM)`);
              return 'int';
            }
            const r = resolve(node.identifier, node.line);
            if (!r.isFunction) {
              // resolve() already reported undefined
            } else {
              diags.add('error', node.line, 1, `'${node.identifier}' is not a defined function`);
            }
            return 'int';
          }
          if (f.params.length !== node.children.length) {
            diags.add('error', node.line, 1,
              `Function '${node.identifier}' expects ${f.params.length} argument(s), got ${node.children.length}`);
          }
          return f.returnType;
        }
        case 'NODE_TERNARY': {
          exprType(node.children[0]);
          const tt = exprType(node.children[1]);
          const et = exprType(node.children[2]);
          return (tt === 'double' || et === 'double') ? 'double' : 'int';
        }
        case 'NODE_CAST': {
          exprType(node.children[0]);
          const tn = node.type_name || 'int';
          if (tn.endsWith('*')) return tn;
          if (tn === 'double' || tn === 'float') return 'double';
          return 'int';
        }
        case 'NODE_SIZEOF': {
          exprType(node.children[0]);
          return 'int';
        }
        case 'NODE_ASSIGNMENT':
        case 'NODE_COMPOUND_ASSIGN':
          return exprType(node.children[0]);
        default:
          return 'int';
      }
    };

    const checkAssignable = (node) => {
      if (!node) return;
      if (node.type === 'NODE_IDENTIFIER' || node.type === 'NODE_INDEX' ||
          node.type === 'NODE_MEMBER') return;
      if (node.type === 'NODE_UNARY_OP' && node.op === '*') return;
      diags.add('error', node.line, 1, 'Assignment to non-lvalue');
    };

    const walkExpr = (node) => {
      if (!node) return;
      if (node.type === 'NODE_IDENTIFIER') {
        resolve(node.identifier, node.line);
        return;
      }
      if (node.type === 'NODE_ASSIGNMENT' || node.type === 'NODE_COMPOUND_ASSIGN') {
        checkAssignable(node.children[0]);
        walkExpr(node.children[0]);
        walkExpr(node.children[1]);
        return;
      }
      if (node.type === 'NODE_FUNC_CALL') {
        exprType(node); // resolves callee, checks arity
        (node.children || []).forEach(walkExpr);
        return;
      }
      (node.children || []).forEach(walkExpr);
    };

    const walkStmt = (node, inLoop, inSwitch) => {
      if (!node) return;
      switch (node.type) {
        case 'NODE_DECL_LIST':
          (node.children || []).forEach((c) => walkStmt(c, inLoop, inSwitch));
          return;
        case 'NODE_VAR_DECL':
          declare(node.identifier, node.type_name, node, false);
          if (node.is_array) (node.has_size ? node.children.slice(1) : node.children).forEach(walkExpr);
          else (node.children || []).forEach(walkExpr);
          return;
        case 'NODE_COMPOUND_STMT':
          scopeStack.push(new Map());
          (node.children || []).forEach((c) => walkStmt(c, inLoop, inSwitch));
          scopeStack.pop();
          return;
        case 'NODE_IF_STMT':
          walkExpr(node.children[0]);
          walkStmt(node.children[1], inLoop, inSwitch);
          if (node.children[2]) walkStmt(node.children[2], inLoop, inSwitch);
          return;
        case 'NODE_WHILE_STMT':
          walkExpr(node.children[0]);
          walkStmt(node.children[1], true, inSwitch);
          return;
        case 'NODE_DO_WHILE_STMT':
          walkStmt(node.children[0], true, inSwitch);
          walkExpr(node.children[1]);
          return;
        case 'NODE_SWITCH_STMT':
          walkExpr(node.children[0]);
          for (let i = 1; i < node.children.length; i++) walkStmt(node.children[i], inLoop, true);
          return;
        case 'NODE_CASE':
        case 'NODE_DEFAULT':
          (node.children || []).forEach((c) => walkStmt(c, inLoop, true));
          return;
        case 'NODE_FOR_STMT':
          scopeStack.push(new Map());
          walkStmt(node.children[0], true, inSwitch);
          walkExpr(node.children[1]);
          walkExpr(node.children[2]);
          walkStmt(node.children[3], true, inSwitch);
          scopeStack.pop();
          return;
        case 'NODE_LABEL_STMT':
          (node.children || []).forEach((c) => walkStmt(c, inLoop, inSwitch));
          return;
        case 'NODE_GOTO':
          return; // label resolution happens during TAC generation
        case 'NODE_BREAK_STMT':
          if (!inLoop && !inSwitch) {
            diags.add('error', node.line, 1, "'break' used outside of a loop or switch");
          }
          return;
        case 'NODE_CONTINUE_STMT':
          if (!inLoop) {
            diags.add('error', node.line, 1, "'continue' used outside of a loop");
          }
          return;
        case 'NODE_RETURN_STMT':
          (node.children || []).forEach(walkExpr);
          return;
        case 'NODE_EXPRESSION_STMT':
          (node.children || []).forEach(walkExpr);
          return;
        default:
          (node.children || []).forEach((c) => walkStmt(c, inLoop, inSwitch));
      }
    };

    const body = frec.node.children.find((c) => c.type !== 'NODE_PARAMETER');
    walkStmt(body, false, false);
    frec.frameSize = nextSlot;
  }

  return { symbols, globals, functions, structs, staticDecls, globalSlotCount: globalSlots.next };
}

// ---------------------------------------------------------------------------
// Phase 4: Three-Address Code generation
// ---------------------------------------------------------------------------

function generateTAC(ast, sem, diags) {
  const instrs = [];
  let tempCount = 0;
  let labelCount = 0;
  // Resolve metadata only in the function currently being generated. Looking
  // through every function frame made a declaration in one function affect an
  // unrelated function that happened to reuse the same identifier.
  let currentFunction = null;
  let generatingGlobalInits = false;
  const bindingScopes = [];
  const tempTypes = new Map(); // temp name -> 'int' | 'double'
  const strings = [];          // string constant pool (printf formats)

  const emit = (op, res, a1, a2, line) => {
    instrs.push({ op, res: res || '', a1: a1 || '', a2: a2 || '', line: line || 0 });
    return instrs[instrs.length - 1];
  };
  const newTemp = (type) => {
    const name = 't' + tempCount++;
    if (type) tempTypes.set(name, type);
    return name;
  };
  const newLabel = () => 'L' + labelCount++;
  const isFloatType = (t) => t === 'float' || t === 'double';

  function constString(v, type) {
    if (isFloatType(type)) {
      if (isIntegral(v)) return String(truncateToInteger(v)) + '.0';
      return cFormatValue(v);
    }
    return String(truncateToInteger(v));
  }

  function placeOfLiteral(node) {
    if (node.type === 'NODE_INT_LITERAL') return { place: String(truncateToInteger(node.num_val)), type: 'int', isConst: true };
    if (node.type === 'NODE_FLOAT_LITERAL') return { place: constString(node.num_val, 'double'), type: 'double', isConst: true };
    return null;
  }

  // Generate code for an expression; returns {place, type, isConst}
  function genExpr(node) {
    if (!node || node.type === 'NODE_ERROR') return { place: '0', type: 'int', isConst: true };

    const lit = placeOfLiteral(node);
    if (lit) return lit;

    if (node.type === 'NODE_STRING_LITERAL') {
      return { place: '"str' + strings.length + '"', type: 'char*', isConst: true, strIdx: internString(node.string_val) };
    }

    if (node.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(node.identifier);
      if (rec && rec.is_array) {
        // array-to-pointer decay: a bare array name in an expression is the
        // address of its first element (&arr[0]). Array parameters decay
        // differently: their slot already holds the caller's address.
        if (rec.isParam) return { place: storageNameOf(node.identifier), type: 'ptr', isConst: false };
        const t = newTemp('ptr');
        emit('ADDR', t, storageNameOf(node.identifier), '0', node.line);
        return { place: t, type: 'ptr', isConst: false };
      }
      return { place: storageNameOf(node.identifier), type: semExprType(node), isConst: false };
    }

    if (node.type === 'NODE_BINARY_OP') {
      const op = node.op;
      if (op === '&&') {
        const a = genExpr(node.children[0]);
        const b = node.children[1];
        const t = newTemp('int');
        const lShort = newLabel();
        emit('=', t, a.place, '', node.line);
        emit('IF_FALSE', lShort, t, '', node.line);
        const bv = genExpr(b);
        emit('=', t, bv.place, '', node.line);
        emit('LABEL', lShort, '', '', node.line);
        emit('!=', t, t, '0', node.line);
        return { place: t, type: 'int', isConst: false };
      }
      if (op === '||') {
        const a = genExpr(node.children[0]);
        const b = node.children[1];
        const t = newTemp('int');
        const lCont = newLabel();
        const lNorm = newLabel();
        emit('=', t, a.place, '', node.line);
        emit('IF_FALSE', lCont, t, '', node.line);
        emit('GOTO', lNorm, '', '', node.line);
        emit('LABEL', lCont, '', '', node.line);
        const bv = genExpr(b);
        emit('=', t, bv.place, '', node.line);
        emit('LABEL', lNorm, '', '', node.line);
        emit('!=', t, t, '0', node.line);
        return { place: t, type: 'int', isConst: false };
      }
      const l = genExpr(node.children[0]);
      const r = genExpr(node.children[1]);
      const isBitwise = ['&', '|', '^', '<<', '>>'].includes(op);
      const resType = ['==', '!=', '<', '>', '<=', '>='].includes(op) || isBitwise
        ? 'int'
        : (isFloatType(l.type) || isFloatType(r.type)) ? 'double' : 'int';
      const t = newTemp(resType);
      emit(op, t, l.place, r.place, node.line);
      return { place: t, type: resType, isConst: false };
    }

    if (node.type === 'NODE_TERNARY') {
      const cond = genExpr(node.children[0]);
      const t = newTemp('int'); // refined after branch codegen below
      const lf = newLabel();
      const le = newLabel();
      emit('IF_FALSE', lf, cond.place, '', node.line);
      const tv = genExpr(node.children[1]);
      emit('=', t, tv.place, '', node.line);
      emit('GOTO', le, '', '', node.line);
      emit('LABEL', lf, '', '', node.line);
      const ev = genExpr(node.children[2]);
      emit('=', t, ev.place, '', node.line);
      emit('LABEL', le, '', '', node.line);
      const resType = (tv.type === 'double' || ev.type === 'double') ? 'double' : 'int';
      tempTypes.set(t, resType);
      return { place: t, type: resType, isConst: false };
    }

    if (node.type === 'NODE_CAST') {
      const v = genExpr(node.children[0]);
      const tn = node.type_name || 'int';
      if (tn.endsWith('*') || tn === 'void') {
        return v; // pointer casts are identity in the educational VM
      }
      const isF = tn === 'double' || tn === 'float';
      const t = newTemp(isF ? 'double' : 'int');
      emit(isF ? 'CAST_F' : 'CAST_I', t, v.place, '', node.line);
      return { place: t, type: isF ? 'double' : 'int', isConst: false };
    }

    if (node.type === 'NODE_SIZEOF') {
      const child = node.children[0];
      if (child && child.type === 'NODE_IDENTIFIER') {
        const rec = findAnySymbol(child.identifier);
        if (rec) {
          if (rec.is_array) return { place: String(rec.size * 4), type: 'int', isConst: true };
          if (typeof rec.type === 'string' && rec.type.endsWith('*')) return { place: '8', type: 'int', isConst: true };
          if (rec.type === 'double') return { place: '8', type: 'int', isConst: true };
          return { place: '4', type: 'int', isConst: true };
        }
      }
      const v = genExpr(child);
      const size = (v.type === 'double' || v.type === 'ptr' || String(v.type).endsWith('*')) ? '8' : '4';
      return { place: size, type: 'int', isConst: true };
    }

    if (node.type === 'NODE_UNARY_OP') {
      if (node.op === '-') {
        const v = genExpr(node.children[0]);
        const t = newTemp(v.type);
        emit('neg', t, v.place, '', node.line);
        return { place: t, type: v.type, isConst: false };
      }
      if (node.op === '!') {
        const v = genExpr(node.children[0]);
        const t = newTemp('int');
        emit('!', t, v.place, '', node.line);
        return { place: t, type: 'int', isConst: false };
      }
      if (node.op === '~') {
        const v = genExpr(node.children[0]);
        const t = newTemp('int');
        emit('~', t, v.place, '', node.line);
        return { place: t, type: 'int', isConst: false };
      }
      if (node.op === '&') {
        const inner = node.children[0];
        if (inner.type === 'NODE_IDENTIFIER') {
          const t = newTemp('ptr');
          emit('ADDR', t, storageNameOf(inner.identifier), '0', inner.line);
          return { place: t, type: 'ptr', isConst: false };
        }
        if (inner.type === 'NODE_INDEX') {
          const addr = genIndexAddr(inner);
          return { place: addr, type: 'ptr', isConst: false };
        }
        if (inner.type === 'NODE_MEMBER') {
          const addr = genMemberAddr(inner);
          if (addr) return { place: addr.place, type: 'ptr', isConst: false };
        }
        const t = newTemp('ptr');
        emit('ADDR', t, '0', '0', node.line);
        return { place: t, type: 'ptr', isConst: false };
      }
      if (node.op === '*') {
        const p = genExpr(node.children[0]);
        const t = newTemp(derefType(node.children[0]));
        emit('LOAD_PTR', t, p.place, '', node.line);
        return { place: t, type: tempTypes.get(t) || 'int', isConst: false };
      }
      if (node.op === '++' || node.op === '--' || node.op === 'p++' || node.op === 'p--') {
        return genIncDec(node);
      }
      const v = genExpr(node.children[0]);
      return v;
    }

    if (node.type === 'NODE_INDEX') {
      const addr = genIndexAddr(node);
      const valueType = indexElementType(node.children[0]);
      const t = newTemp(valueType);
      emit('LOAD_PTR', t, addr, '', node.line);
      return { place: t, type: valueType, isConst: false };
    }

    if (node.type === 'NODE_MEMBER') {
      const addr = genMemberAddr(node);
      if (!addr) return { place: '0', type: 'int', isConst: true };
      // Array members decay to their first element's address. Loading here
      // returned element zero and then treated that value as a pointer for
      // `s.items[i]` and printf("%s", s.text).
      if (addr.isArray) {
        return { place: addr.place, type: addr.fieldType, isConst: false };
      }
      const t = newTemp(addr.fieldType === 'double' || addr.fieldType === 'float' ? 'double' : 'int');
      emit('LOAD_PTR', t, addr.place, '', node.line);
      return { place: t, type: tempTypes.get(t) || 'int', isConst: false };
    }

    if (node.type === 'NODE_FUNC_CALL') {
      return genCall(node);
    }

    if (node.type === 'NODE_ASSIGNMENT' || node.type === 'NODE_COMPOUND_ASSIGN') {
      const value = genAssignment(node);
      return value;
    }

    return { place: '0', type: 'int', isConst: true };
  }

  function internString(s) {
    strings.push(s);
    return strings.length - 1;
  }

  function semExprType(node) {
    // lightweight type lookup consistent with semantic pass
    if (node.type !== 'NODE_IDENTIFIER') return 'int';
    const rec = findAnySymbol(node.identifier);
    return rec ? rec.type : 'int';
  }

  function findAnySymbol(name) {
    if (!generatingGlobalInits) {
      for (let i = bindingScopes.length - 1; i >= 0; i--) {
        if (bindingScopes[i].has(name)) return bindingScopes[i].get(name);
      }
    }
    if (sem.globals.has(name)) return sem.globals.get(name);
    return null;
  }

  const storageNameOf = (name) => {
    const rec = findAnySymbol(name);
    return rec && rec.storageName ? rec.storageName : name;
  };

  const declarationStorage = (node) => {
    if (generatingGlobalInits) return node.identifier;
    return node && node._binding && node._binding.storageName
      ? node._binding.storageName : node.identifier;
  };

  const normalizedValueType = (type) => {
    if (type === 'double' || type === 'float') return 'double';
    if (typeof type === 'string' && type.endsWith('*')) return 'ptr';
    return 'int';
  };

  // Resolve member metadata without emitting TAC. This is used for type
  // propagation, notably so division involving a double array element uses
  // floating-point rather than truncating integer semantics.
  function memberFieldInfo(node) {
    if (!node || node.type !== 'NODE_MEMBER') return null;
    const base = node.children[0];
    let structName = null;
    if (base && base.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(base.identifier);
      if (rec && typeof rec.type === 'string' && rec.type.startsWith('struct ')) {
        structName = rec.type.slice(7);
      }
    } else if (base && base.type === 'NODE_MEMBER') {
      const parent = memberFieldInfo(base);
      if (parent && !parent.is_array && typeof parent.type === 'string' && parent.type.startsWith('struct ')) {
        structName = parent.type.slice(7);
      }
    }
    const s = structName ? sem.structs.get(structName) : null;
    return s && s.fields.has(node.identifier) ? s.fields.get(node.identifier) : null;
  }

  function indexElementType(base) {
    if (!base) return 'int';
    if (base.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(base.identifier);
      if (rec) {
        if (rec.is_array) return normalizedValueType(rec.type);
        if (typeof rec.type === 'string' && rec.type.endsWith('*')) {
          return normalizedValueType(rec.type.slice(0, -1));
        }
      }
    } else if (base.type === 'NODE_MEMBER') {
      const field = memberFieldInfo(base);
      if (field) {
        if (field.is_array) return normalizedValueType(field.type);
        if (typeof field.type === 'string' && field.type.endsWith('*')) {
          return normalizedValueType(field.type.slice(0, -1));
        }
      }
    }
    return 'int';
  }

  function derefType(ptrNode) {
    if (ptrNode.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(ptrNode.identifier);
      if (rec && typeof rec.type === 'string' && rec.type.endsWith('*')) return rec.type.slice(0, -1);
    }
    return 'int';
  }

  function genIndexAddr(node) {
    const base = node.children[0];
    const idx = genExpr(node.children[1]);
    const t = newTemp('ptr');
    if (base.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(base.identifier);
      if (rec && rec.is_array && !rec.isParam) {
        // fixed array: bounds-checked IDX_ADDR on the array slot
        emit('IDX_ADDR', t, storageNameOf(base.identifier), idx.place, node.line);
        return t;
      }
      // pointer variable or array parameter: the slot holds an address.
      // Load its value and index (no bounds check — C pointer semantics).
      emit('+', t, storageNameOf(base.identifier), idx.place, node.line);
      return t;
    }
    // pointer-valued expression: evaluate it, then index
    const bp = genExpr(base);
    emit('+', t, bp.place, idx.place, node.line);
    return t;
  }

  function genMemberAddr(node) {
    const base = node.children[0];
    if (base && base.type === 'NODE_IDENTIFIER') {
      const rec = findAnySymbol(base.identifier);
      if (rec && typeof rec.type === 'string' && rec.type.startsWith('struct ')) {
        const s = sem.structs.get(rec.type.slice(7));
        if (s && s.fields.has(node.identifier)) {
          const f = s.fields.get(node.identifier);
          const t = newTemp('ptr');
          emit('ADDR', t, storageNameOf(base.identifier), String(f.offset), node.line);
          return { place: t, fieldType: f.is_array ? f.type + '*' : f.type, isArray: !!f.is_array };
        }
      }
    }
    if (base && base.type === 'NODE_MEMBER') {
      // nested struct member: o.in.a — resolve the base member first, then
      // add this field's offset relative to the base's struct type
      const bma = genMemberAddr(base);
      if (bma && typeof bma.fieldType === 'string' && bma.fieldType.startsWith('struct ')) {
        const s = sem.structs.get(bma.fieldType.slice(7));
        if (s && s.fields.has(node.identifier)) {
          const f = s.fields.get(node.identifier);
          const t = newTemp('ptr');
          emit('+', t, bma.place, String(f.offset), node.line);
          return { place: t, fieldType: f.is_array ? f.type + '*' : f.type, isArray: !!f.is_array };
        }
      }
    }
    diags.add('error', node.line, 1, 'Invalid member access');
    return null;
  }

  function genCall(node) {
    const isBuiltinIO = node.identifier === 'printf' || node.identifier === 'scanf';
    if (isBuiltinIO) {
      const first = node.children[0];
      let fmt = '';
      if (first && first.type === 'NODE_STRING_LITERAL') {
        fmt = first.string_val;
      } else if (first) {
        diags.add('error', node.line, 1, `${node.identifier} format must be a string literal`);
      }
      const idx = internString(fmt);
      // value/address arguments only — the format string is referenced by index
      for (let i = 1; i < node.children.length; i++) {
        const a = genExpr(node.children[i]);
        emit('PARAM', '', a.place, '', node.line);
      }
      emit(isBuiltinIO && node.identifier === 'printf' ? 'PRINT' : 'READ',
        node.identifier, '"str' + idx + '"', String(node.children.length - 1), node.line);
      return { place: '', type: 'int', isConst: false };
    }
    // User function/builtin arguments are pushed left-to-right. Preserve the
    // declared return type on the result temp; otherwise `sqrt(2.0) / 2` and a
    // double-returning user function incorrectly selected integer division.
    for (const arg of node.children) {
      const a = genExpr(arg);
      emit('PARAM', '', a.place, '', node.line);
    }
    const builtin = BUILTIN_FUNCTIONS.find((b) => b.name === node.identifier);
    const fn = sem.functions.get(node.identifier);
    const declaredType = builtin ? builtin.type : (fn ? fn.returnType : 'int');
    const resultType = normalizedValueType(declaredType);
    const t = newTemp(resultType);
    emit('CALL', t, node.identifier, String(node.children.length), node.line);
    return { place: t, type: resultType, isConst: false };
  }

  // Compute address place for an lvalue; returns {mode:'direct',name} or {mode:'addr',place}
  function lvalueAddr(node) {
    if (node.type === 'NODE_IDENTIFIER') {
      return { mode: 'direct', name: storageNameOf(node.identifier), line: node.line };
    }
    if (node.type === 'NODE_INDEX') {
      return {
        mode: 'addr', place: genIndexAddr(node),
        type: indexElementType(node.children[0]), line: node.line
      };
    }
    if (node.type === 'NODE_MEMBER') {
      const addr = genMemberAddr(node);
      return addr ? {
        mode: 'addr', place: addr.place,
        type: addr.isArray ? 'ptr' : normalizedValueType(addr.fieldType),
        line: node.line
      } : null;
    }
    if (node.type === 'NODE_UNARY_OP' && node.op === '*') {
      const p = genExpr(node.children[0]);
      return { mode: 'addr', place: p.place, type: normalizedValueType(derefType(node.children[0])), line: node.line };
    }
    return null;
  }

  function storeToLvalue(target, valuePlace, line) {
    if (!target) return;
    if (target.mode === 'direct') {
      emit('=', target.name, valuePlace, '', line);
    } else {
      emit('STORE_PTR', '', target.place, valuePlace, line);
    }
  }

  function genAssignment(node) {
    const target = node.children[0];
    if (node.type === 'NODE_ASSIGNMENT') {
      // evaluate RHS first (C semantics: RHS before store)
      const rhs = genExpr(node.children[1]);
      const lv = lvalueAddr(target);
      storeToLvalue(lv, rhs.place, node.line);
      return rhs;
    }
    // compound: read lvalue once, apply op, store back
    const opMap = { '+=': '+', '-=': '-', '*=': '*', '/=': '/', '%=': '%',
                    '&=': '&', '|=': '|', '^=': '^', '<<=': '<<', '>>=': '>>' };
    const op = opMap[node.op];
    const lv = lvalueAddr(target);
    if (!lv) return { place: '0', type: 'int' };
    const oldV = lv.mode === 'direct'
      ? { place: lv.name, type: semExprType(target) }
      : (() => { const type = lv.type || 'int'; const t = newTemp(type); emit('LOAD_PTR', t, lv.place, '', node.line); return { place: t, type }; })();
    const rhs = genExpr(node.children[1]);
    const resType = (isFloatType(oldV.type) || isFloatType(rhs.type)) ? 'double' : 'int';
    const t = newTemp(resType);
    emit(op, t, oldV.place, rhs.place, node.line);
    storeToLvalue(lv, t, node.line);
    return { place: t, type: resType, isConst: false };
  }

  function genIncDec(node) {
    const op = (node.op === '++' || node.op === 'p++') ? '+' : '-';
    const prefix = node.op === '++' || node.op === '--';
    const target = node.children[0];
    const lv = lvalueAddr(target);
    if (!lv) return { place: '0', type: 'int', isConst: true };
    const oldV = lv.mode === 'direct'
      ? { place: lv.name, type: semExprType(target) }
      : (() => { const type = lv.type || 'int'; const t = newTemp(type); emit('LOAD_PTR', t, lv.place, '', node.line); return { place: t, type }; })();
    const t = newTemp(oldV.type === 'double' ? 'double' : 'int');
    emit(op, t, oldV.place, '1', node.line);
    storeToLvalue(lv, t, node.line);
    if (prefix) return { place: t, type: oldV.type === 'double' ? 'double' : 'int', isConst: false };
    return oldV;
  }

  // ---- statements ----
  const loopStack = [];

  // per-function goto-label registry (pre-scanned so forward gotos work)
  let labelMap = new Map();
  function collectLabels(node) {
    if (!node) return;
    if (node.type === 'NODE_LABEL_STMT' && !labelMap.has(node.identifier)) {
      labelMap.set(node.identifier, newLabel());
    }
    (node.children || []).forEach(collectLabels);
  }
  function registerLabel(name) {
    if (!labelMap.has(name)) labelMap.set(name, newLabel());
    return labelMap.get(name);
  }
  function findLabel(name) {
    return labelMap.has(name) ? labelMap.get(name) : null;
  }

  // Emits initializer code for a variable declaration (arrays or scalar).
  function emitVarDeclInit(node) {
    const declPlace = declarationStorage(node);
    if (node.is_array) {
      // children[0] = size literal when has_size, rest = initializer values
      const inits = node.has_size ? node.children.slice(1) : node.children.slice(0);
      const rec = findAnySymbol(node.identifier);
      const cnt = (rec && rec.is_array) ? rec.size : node.children.length;
      if (inits.length === 1 && inits[0].type === 'NODE_STRING_LITERAL') {
        // char s[N] = "literal": copy the characters (and the NUL when it
        // fits) into the array cells, then zero-fill the remainder.
        const str = String(inits[0].string_val || '');
        const n = str.length + 1; // chars + NUL
        const storeByte = (i, ch) => {
          const idxT = newTemp('ptr');
          emit('IDX_ADDR', idxT, declPlace, String(i), node.line);
          emit('STORE_PTR', '', idxT, String(ch), node.line);
        };
        for (let i = 0; i < n && i < cnt; i++) storeByte(i, i < str.length ? str.charCodeAt(i) : 0);
        for (let i = n; i < cnt; i++) storeByte(i, 0);
        return;
      }
      inits.forEach((initExpr, i) => {
        const v = genExpr(initExpr);
        const idxT = newTemp('ptr');
        emit('IDX_ADDR', idxT, declPlace, String(i), node.line);
        emit('STORE_PTR', '', idxT, v.place, node.line);
      });
      // C semantics: the remainder of a partially-initialized aggregate is
      // zero-filled every time the declaration executes.
      for (let i = inits.length; i < cnt; i++) {
        const idxT = newTemp('ptr');
        emit('IDX_ADDR', idxT, declPlace, String(i), node.line);
        emit('STORE_PTR', '', idxT, '0', node.line);
      }
      return;
    }
    if (node.children.length > 0 && node.type_name && node.type_name.startsWith('struct ')) {
      // Struct initializer list. Keep array-group metadata alongside each leaf
      // so a string literal initializes one whole char array subobject before
      // the next initializer advances to the following field.
      const leaves = [];
      const flatten = (structName, base) => {
        const s = sem.structs.get(structName);
        if (!s) return;
        for (const f of s.fields.values()) {
          if (typeof f.type === 'string' && f.type.startsWith('struct ')) {
            if (!f.is_array) flatten(f.type.slice(7), base + f.offset);
            continue;
          }
          if (f.is_array) {
            const start = base + f.offset;
            for (let k = 0; k < f.size && leaves.length < 1024; k++) {
              leaves.push({ offset: start + k, arrayStart: start, arraySize: f.size });
            }
            continue;
          }
          leaves.push({ offset: base + f.offset, arrayStart: -1, arraySize: 0 });
        }
      };
      flatten(node.type_name.slice(7), 0);
      const storeOffset = (offset, valPlace) => {
        const t = newTemp('ptr');
        emit('ADDR', t, declPlace, String(offset), node.line);
        emit('STORE_PTR', '', t, valPlace, node.line);
      };
      let cursor = 0;
      for (const initExpr of node.children) {
        if (cursor >= leaves.length) break;
        const leaf = leaves[cursor];
        if (initExpr.type === 'NODE_STRING_LITERAL' &&
            leaf.arraySize > 0 && leaf.offset === leaf.arrayStart) {
          const str = String(initExpr.string_val || '');
          const copied = Math.min(str.length + 1, leaf.arraySize);
          for (let k = 0; k < copied; k++) {
            storeOffset(leaf.arrayStart + k, k < str.length ? String(str.charCodeAt(k)) : '0');
          }
          for (let k = copied; k < leaf.arraySize; k++) storeOffset(leaf.arrayStart + k, '0');
          cursor += leaf.arraySize;
          continue;
        }
        const v = genExpr(initExpr);
        storeOffset(leaf.offset, v.place);
        cursor++;
      }
      for (; cursor < leaves.length; cursor++) storeOffset(leaves[cursor].offset, '0');
      return;
    }
    if (node.children.length > 0) {
      const v = genExpr(node.children[0]);
      emit('=', declPlace, v.place, '', node.line);
    }
  }

  function genVarDecl(node) {
    // static locals are initialized exactly once in the global-init prologue,
    // never on each call
    if (node.is_static) return;
    emitVarDeclInit(node);
  }

  function bindDeclaration(node) {
    if (!node || !node._binding || bindingScopes.length === 0) return;
    bindingScopes[bindingScopes.length - 1].set(node.identifier, node._binding);
  }

  function genStmt(node) {
    if (!node || node.type === 'NODE_EMPTY' || node.type === 'NODE_ERROR') return;

    if (node.type === 'NODE_DECL_LIST') { node.children.forEach(genStmt); return; }

    if (node.type === 'NODE_VAR_DECL') {
      bindDeclaration(node); // a C declarator is in scope for its initializer
      genVarDecl(node);
      return;
    }

    if (node.type === 'NODE_COMPOUND_STMT') {
      bindingScopes.push(new Map());
      node.children.forEach(genStmt);
      bindingScopes.pop();
      return;
    }

    if (node.type === 'NODE_EXPRESSION_STMT') {
      node.children.forEach((e) => genExpr(e));
      return;
    }

    if (node.type === 'NODE_IF_STMT') {
      const cond = genExpr(node.children[0]);
      const lElse = newLabel();
      const lEnd = newLabel();
      emit('IF_FALSE', lElse, cond.place, '', node.line);
      genStmt(node.children[1]);
      if (node.children[2]) {
        emit('GOTO', lEnd, '', '', node.line);
        emit('LABEL', lElse, '', '', node.line);
        genStmt(node.children[2]);
        emit('LABEL', lEnd, '', '', node.line);
      } else {
        emit('LABEL', lElse, '', '', node.line);
      }
      return;
    }

    if (node.type === 'NODE_WHILE_STMT') {
      const lStart = newLabel();
      const lEnd = newLabel();
      loopStack.push({ brk: lEnd, cont: lStart, isSwitch: false });
      emit('LABEL', lStart, '', '', node.line);
      const cond = genExpr(node.children[0]);
      emit('IF_FALSE', lEnd, cond.place, '', node.line);
      genStmt(node.children[1]);
      emit('GOTO', lStart, '', '', node.line);
      emit('LABEL', lEnd, '', '', node.line);
      loopStack.pop();
      return;
    }

    if (node.type === 'NODE_DO_WHILE_STMT') {
      const lStart = newLabel();
      const lEnd = newLabel();
      loopStack.push({ brk: lEnd, cont: lStart, isSwitch: false });
      emit('LABEL', lStart, '', '', node.line);
      genStmt(node.children[0]);
      const cond = genExpr(node.children[1]);
      emit('IF_FALSE', lEnd, cond.place, '', node.line);
      emit('GOTO', lStart, '', '', node.line);
      emit('LABEL', lEnd, '', '', node.line);
      loopStack.pop();
      return;
    }

    if (node.type === 'NODE_SWITCH_STMT') {
      const sel = genExpr(node.children[0]);
      const lEnd = newLabel();
      loopStack.push({ brk: lEnd, cont: null, isSwitch: true });
      // jump table: test each case in order
      const caseLabels = [];
      let defaultLabel = null;
      // first pass: create labels
      for (let i = 1; i < node.children.length; i++) {
        const c = node.children[i];
        caseLabels.push(newLabel());
        if (c.type === 'NODE_DEFAULT') defaultLabel = caseLabels[caseLabels.length - 1];
      }
      for (let i = 1; i < node.children.length; i++) {
        const c = node.children[i];
        if (c.type === 'NODE_CASE') {
          const t = newTemp('int');
          emit('==', t, sel.place, String(truncateToInteger(c.num_val)), c.line);
          const lNext = newLabel();
          emit('IF_FALSE', lNext, t, '', c.line);
          emit('GOTO', caseLabels[i - 1], '', '', c.line);
          emit('LABEL', lNext, '', '', c.line);
        }
      }
      emit('GOTO', defaultLabel || lEnd, '', '', node.line);
      // second pass: bodies
      for (let i = 1; i < node.children.length; i++) {
        const c = node.children[i];
        emit('LABEL', caseLabels[i - 1], '', '', c.line);
        for (const st of c.children) genStmt(st);
      }
      emit('LABEL', lEnd, '', '', node.line);
      loopStack.pop();
      return;
    }

    if (node.type === 'NODE_GOTO') {
      const lbl = findLabel(node.identifier);
      if (lbl) emit('GOTO', lbl, '', '', node.line);
      else diags.add('error', node.line, 1, `Use of undefined label '${node.identifier}'`);
      return;
    }

    if (node.type === 'NODE_LABEL_STMT') {
      const lbl = registerLabel(node.identifier);
      emit('LABEL', lbl, '', '', node.line);
      if (node.children[0]) genStmt(node.children[0]);
      return;
    }

    if (node.type === 'NODE_FOR_STMT') {
      bindingScopes.push(new Map());
      const [init, cond, incr, body] = node.children;
      const lStart = newLabel();
      const lStep = newLabel();
      const lEnd = newLabel();
      genStmt(init);
      loopStack.push({ brk: lEnd, cont: lStep, isSwitch: false });
      emit('LABEL', lStart, '', '', node.line);
      if (cond.type !== 'NODE_EMPTY') {
        const c = genExpr(cond);
        emit('IF_FALSE', lEnd, c.place, '', node.line);
      }
      genStmt(body);
      emit('LABEL', lStep, '', '', node.line);
      if (incr.type !== 'NODE_EMPTY') genExpr(incr);
      emit('GOTO', lStart, '', '', node.line);
      emit('LABEL', lEnd, '', '', node.line);
      loopStack.pop();
      bindingScopes.pop();
      return;
    }

    if (node.type === 'NODE_BREAK_STMT') {
      const loop = loopStack[loopStack.length - 1];
      if (loop) emit('GOTO', loop.brk, '', '', node.line);
      return;
    }
    if (node.type === 'NODE_CONTINUE_STMT') {
      // continue targets the innermost *loop*, skipping any enclosing switch
      for (let i = loopStack.length - 1; i >= 0; i--) {
        if (!loopStack[i].isSwitch && loopStack[i].cont) {
          emit('GOTO', loopStack[i].cont, '', '', node.line);
          return;
        }
      }
      return;
    }

    if (node.type === 'NODE_RETURN_STMT') {
      if (node.children.length > 0) {
        const v = genExpr(node.children[0]);
        emit('RETURN', '', v.place, '', node.line);
      } else {
        emit('RETURN', '', '', '', node.line);
      }
      return;
    }
  }

  // Global (file-scope) initializers run once, at the very start of main.
  // Static locals were promoted to global storage; their initializers also run
  // here (exactly once), never on each call.
  function emitGlobalInits() {
    generatingGlobalInits = true;
    for (const top of ast.children) {
      const decls = top.type === 'NODE_DECL_LIST' ? top.children : [top];
      for (const d of decls) {
        if (!d || d.type !== 'NODE_VAR_DECL') continue;
        emitVarDeclInit(d);
      }
    }
    for (const d of (sem.staticDecls || [])) {
      emitVarDeclInit(d);
    }
    generatingGlobalInits = false;
  }

  for (const top of ast.children) {
    if (top.type !== 'NODE_FUNCTION_DEF') continue;
    if (top.is_forward) continue; // forward declaration — no body
    currentFunction = top.identifier;
    bindingScopes.length = 0;
    bindingScopes.push(new Map());
    for (const param of top.children.filter((c) => c.type === 'NODE_PARAMETER')) bindDeclaration(param);
    emit('FUNC_BEGIN', top.identifier, '', '', top.line);
    labelMap = new Map();
    const body = top.children.find((c) => c.type !== 'NODE_PARAMETER');
    collectLabels(body);
    if (top.identifier === 'main') emitGlobalInits();
    genStmt(body);
    // implicit return when control reaches the end of the body
    const last = instrs[instrs.length - 1];
    if (!last || last.op !== 'RETURN') {
      emit('RETURN', '', top.identifier === 'main' ? '0' : '', '', top.line);
    }
    emit('FUNC_END', top.identifier, '', '', top.line);
    bindingScopes.length = 0;
    currentFunction = null;
  }

  return { instrs, tempTypes, strings, tempCount, labelCount };
}

// ---------------------------------------------------------------------------
// Phase 5: Optimizer (constant folding, constant propagation, strength
// reduction, dead code elimination) — real rewrites with real metrics.
// ---------------------------------------------------------------------------

const BIN_OPS = new Set(['+', '-', '*', '/', '%', '==', '!=', '<', '>', '<=', '>=', '&', '|', '^', '<<', '>>']);

function parseTacNumber(s) {
  if (!/^-?\d+$/.test(s)) return null;
  return parseInt(s, 10);
}

function isPlainPlace(s) {
  return /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(s);
}

function optimizeTAC(instrs, tempTypes) {
  const metrics = { constant_fold: 0, constant_prop: 0, dead_code: 0, strength_reduce: 0, reduction_percentage: 0 };

  let list = instrs.map((i) => Object.assign({}, i));

  const computeBin = (op, a, b, floatMode) => {
    switch (op) {
      case '+': return a + b;
      case '-': return a - b;
      case '*': return a * b;
      case '/':
        if (b === 0) return null;
        return floatMode ? a / b : Math.trunc(a / b);
      case '%':
        if (b === 0) return null;
        return a % b;
      case '==': return a === b ? 1 : 0;
      case '!=': return a !== b ? 1 : 0;
      case '<': return a < b ? 1 : 0;
      case '>': return a > b ? 1 : 0;
      case '<=': return a <= b ? 1 : 0;
      case '>=': return a >= b ? 1 : 0;
      // bitwise: int32 semantics, identical to the VM
      case '&': return (a | 0) & (b | 0);
      case '|': return (a | 0) | (b | 0);
      case '^': return (a | 0) ^ (b | 0);
      case '<<': return (a | 0) << (b | 0);
      case '>>': return (a | 0) >> (b | 0);
      default: return null;
    }
  };

  // Pass 1: constant folding of binary ops on two integer literals
  for (const ins of list) {
    if (BIN_OPS.has(ins.op)) {
      const a = parseTacNumber(ins.a1);
      const b = parseTacNumber(ins.a2);
      if (a !== null && b !== null) {
        const r = computeBin(ins.op, a, b, false);
        if (r !== null) {
          ins.op = '=';
          ins.a1 = String(r);
          ins.a2 = '';
          metrics.constant_fold++;
        }
      }
    } else if (ins.op === 'neg') {
      const a = parseTacNumber(ins.a1);
      if (a !== null) {
        ins.op = '=';
        ins.a1 = String(-a);
        metrics.constant_fold++;
      }
    } else if (ins.op === '~') {
      const a = parseTacNumber(ins.a1);
      if (a !== null) {
        ins.op = '=';
        ins.a1 = String(~(a | 0));
        metrics.constant_fold++;
      }
    }
  }

  // Pass 2: constant propagation for temps assigned exactly once from a constant
  const constOf = new Map();
  const assignCount = new Map();
  for (const ins of list) {
    if (ins.op === '=' && isPlainPlace(ins.res) && ins.res.startsWith('t') && parseTacNumber(ins.a1) !== null) {
      constOf.set(ins.res, ins.a1);
    }
    if (isPlainPlace(ins.res) && ins.res.startsWith('t')) {
      assignCount.set(ins.res, (assignCount.get(ins.res) || 0) + 1);
    }
  }
  for (const [k, v] of constOf) {
    if (assignCount.get(k) !== 1) constOf.delete(k);
  }
  const substitute = (p) => {
    if (constOf.has(p)) { metrics.constant_prop++; return constOf.get(p); }
    return p;
  };
  for (const ins of list) {
    if (ins.op === 'LABEL' || ins.op === 'FUNC_BEGIN' || ins.op === 'FUNC_END') continue;
    ins.a1 = substitute(ins.a1);
    ins.a2 = substitute(ins.a2);
  }

  // Pass 3: strength reduction (x * 2 -> x + x)
  for (const ins of list) {
    if (ins.op === '*') {
      if (ins.a2 === '2') {
        ins.op = '+';
        ins.a2 = ins.a1;
        metrics.strength_reduce++;
      } else if (ins.a1 === '2') {
        ins.op = '+';
        const other = ins.a2;
        ins.a1 = other;
        ins.a2 = other;
        metrics.strength_reduce++;
      }
    }
  }

  // Pass 4: dead code elimination — remove pure assignments to temps never used again
  for (;;) {
    const used = new Set();
    for (const ins of list) {
      if (isPlainPlace(ins.a1) && ins.a1.startsWith('t')) used.add(ins.a1);
      if (isPlainPlace(ins.a2) && ins.a2.startsWith('t')) used.add(ins.a2);
    }
    const pureOps = new Set(['=', '+', '-', '*', '/', '%', 'neg', '!', '~', '==', '!=', '<', '>', '<=', '>=', '&', '|', '^', '<<', '>>', 'CAST_I', 'CAST_F']);
    let removedAny = false;
    const next = [];
    for (const ins of list) {
      if (pureOps.has(ins.op) && isPlainPlace(ins.res) && ins.res.startsWith('t') && !used.has(ins.res)) {
        metrics.dead_code++;
        removedAny = true;
        continue;
      }
      next.push(ins);
    }
    list = next;
    if (!removedAny) break;
  }

  const original = instrs.length;
  metrics.reduction_percentage = original > 0
    ? Math.round((1 - list.length / original) * 1000) / 10
    : 0;

  return { optTac: list, metrics };
}

// ---------------------------------------------------------------------------
// Phase 6: Bytecode generation
// ---------------------------------------------------------------------------

function generateBytecode(optTac, sem, tempTypes, strings) {
  const code = [];
  const labelPC = new Map();
  const funcPC = new Map();
  const fixups = []; // {index, label}
  const currentFunc = { name: null };
  const tempsByFunc = new Map(); // funcName -> number of temp slots used

  // slot allocation: globals already laid out; temps get per-function slots
  const tempSlots = new Map(); // funcName -> Map(temp -> offset)
  const placeInfo = (place) => {
    // returns {isGlobal, slot} for named places
    if (sem.globals.has(place)) {
      const g = sem.globals.get(place);
      return { isGlobal: true, slot: g.offset, size: g.size };
    }
    const f = sem.functions.get(currentFunc.name);
    if (f) {
      if (f.storage && f.storage.has(place)) {
        const r = f.storage.get(place);
        return { isGlobal: !!r.isGlobal, slot: r.offset, size: r.size };
      }
      if (f.frame.has(place)) {
        const r = f.frame.get(place);
        return { isGlobal: !!r.isGlobal, slot: r.offset, size: r.size };
      }
      let temps = tempSlots.get(currentFunc.name);
      if (!temps) { temps = new Map(); tempSlots.set(currentFunc.name, temps); }
      if (!temps.has(place)) temps.set(place, f.frameSize + temps.size);
      return { isGlobal: false, slot: temps.get(place), size: 1 };
    }
    return { isGlobal: true, slot: 0, size: 1 };
  };

  const emitInstr = (op, operand, symbol, line, extra) => {
    code.push(Object.assign({ pc: code.length, op, operand: operand || 0, symbol: symbol || '', line: line || 0 }, extra || {}));
    return code[code.length - 1];
  };

  // Accepts C numeric literal shapes as emitted by constString, including
  // scientific notation: [-+]?digits[.digits][eE[+-]digits]
  const isNumericPlace = (p) => /^-?[0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?$/.test(p);

  const pushPlace = (place, line) => {
    if (isNumericPlace(place)) {
      emitInstr('PUSH', parseFloat(place), place, line);
    } else if (place.startsWith('"str')) {
      emitInstr('PUSH_STR', parseInt(place.slice(4, -1), 10), place, line);
    } else {
      const info = placeInfo(place);
      emitInstr('LOAD', 0, place, line, { slot: info.slot, isGlobal: info.isGlobal });
    }
  };

  const arraySizeOf = (name) => {
    if (sem.globals.has(name)) {
      const g = sem.globals.get(name);
      return g.is_array ? g.size : -1;
    }
    const f = sem.functions.get(currentFunc.name);
    if (f && f.storage && f.storage.has(name)) {
      const r = f.storage.get(name);
      return r.is_array ? r.size : -1;
    }
    if (f && f.frame.has(name)) {
      const r = f.frame.get(name);
      return r.is_array ? r.size : -1;
    }
    return -1;
  };

  for (const ins of optTac) {
    switch (ins.op) {
      case 'FUNC_BEGIN':
        currentFunc.name = ins.res;
        funcPC.set(ins.res, code.length);
        break;
      case 'FUNC_END':
        tempsByFunc.set(ins.res, (tempSlots.get(ins.res) || new Map()).size);
        currentFunc.name = null;
        break;
      case 'LABEL':
        labelPC.set(ins.res, code.length);
        break;
      case 'GOTO': {
        const i = emitInstr('JMP', 0, ins.res, ins.line, { target: ins.res });
        fixups.push({ index: i.pc, label: ins.res });
        break;
      }
      case 'IF_FALSE': {
        pushPlace(ins.a1, ins.line);
        const i = emitInstr('JZ', 0, ins.res, ins.line, { target: ins.res });
        fixups.push({ index: i.pc, label: ins.res });
        break;
      }
      case '=': {
        pushPlace(ins.a1, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case '+': case '-': case '*': case '/': case '%':
      case '==': case '!=': case '<': case '>': case '<=': case '>=':
      case '&&': case '||':
      case '&': case '|': case '^': case '<<': case '>>': {
        pushPlace(ins.a1, ins.line);
        pushPlace(ins.a2, ins.line);
        let op = { '+': 'ADD', '-': 'SUB', '*': 'MUL', '%': 'MOD', '==': 'EQ', '!=': 'NEQ',
                   '<': 'LT', '>': 'GT', '<=': 'LEQ', '>=': 'GEQ', '&&': 'AND', '||': 'OR',
                   '&': 'BAND', '|': 'BOR', '^': 'BXOR', '<<': 'SHL', '>>': 'SHR' }[ins.op];
        if (ins.op === '/') {
          const lt = tempTypes.get(ins.a1);
          const rt = tempTypes.get(ins.a2);
          const floaty = (v) => /^-?\d+\.\d+$/.test(v) || lt === 'double' || rt === 'double';
          op = floaty(ins.a1) || floaty(ins.a2) ? 'DIVF' : 'DIV';
        }
        emitInstr(op, 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'neg': {
        pushPlace(ins.a1, ins.line);
        emitInstr('NEG', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case '!': {
        pushPlace(ins.a1, ins.line);
        emitInstr('NOT', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case '~': {
        pushPlace(ins.a1, ins.line);
        emitInstr('BNOT', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'CAST_I': {
        pushPlace(ins.a1, ins.line);
        emitInstr('CVT_I', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'CAST_F': {
        pushPlace(ins.a1, ins.line);
        emitInstr('CVT_F', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'ADDR': {
        const info = placeInfo(ins.a1);
        const off = parseTacNumber(ins.a2) || 0;
        emitInstr('ADDR', off, ins.a1, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        const tinfo = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: tinfo.slot, isGlobal: tinfo.isGlobal });
        break;
      }
      case 'IDX_ADDR': {
        const info = placeInfo(ins.a1);
        pushPlace(ins.a2, ins.line); // index
        emitInstr('IDX_ADDR', arraySizeOf(ins.a1), ins.a1, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        const tinfo = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: tinfo.slot, isGlobal: tinfo.isGlobal });
        break;
      }
      case 'LOAD_PTR': {
        pushPlace(ins.a1, ins.line);
        emitInstr('LOAD_AT', 0, ins.res, ins.line);
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'STORE_PTR': {
        // TAC: a1 = address place, a2 = value place.
        // Push value first, then address, so the VM pops address then value.
        pushPlace(ins.a2, ins.line);
        pushPlace(ins.a1, ins.line);
        emitInstr('STORE_AT', 0, ins.a1, ins.line);
        break;
      }
      case 'PARAM': {
        pushPlace(ins.a1, ins.line);
        break;
      }
      case 'CALL': {
        // CALL always carries a result temp (void calls store the implicit 0).
        emitInstr('CALL', parseInt(ins.a2, 10), ins.a1, ins.line, { target: ins.a1 });
        const info = placeInfo(ins.res);
        emitInstr('STORE', 0, ins.res, ins.line, { slot: info.slot, isGlobal: info.isGlobal });
        break;
      }
      case 'PRINT': {
        const idx = parseInt(ins.res === 'printf' ? ins.a1.slice(4, -1) : '0', 10);
        emitInstr('PRINT', parseInt(ins.a2, 10), ins.a1, ins.line, { fmtIdx: idx });
        break;
      }
      case 'READ': {
        const idx = parseInt(ins.a1.slice(4, -1), 10);
        emitInstr('INPUT', parseInt(ins.a2, 10), ins.a1, ins.line, { fmtIdx: idx });
        break;
      }
      case 'RETURN': {
        if (ins.a1 && ins.a1 !== '') pushPlace(ins.a1, ins.line);
        emitInstr('RET', ins.a1 && ins.a1 !== '' ? 1 : 0, '', ins.line);
        break;
      }
      default:
        break;
    }
  }

  emitInstr('HALT', 0, '', 0);

  for (const f of fixups) {
    const target = labelPC.get(f.label);
    code[f.index].operand = target !== undefined ? target : code.length - 1;
  }

  return { code, funcPC, strings, tempsByFunc };
}

// ---------------------------------------------------------------------------
// Float formatting helpers (must match the C backend byte-for-byte)
// ---------------------------------------------------------------------------

// Mirrors C fmt.c format_value: the shortest %g representation that
// round-trips to the same double (exponent leading zeros stripped).
function cFormatValue(v) {
  if (isIntegral(v)) return String(truncateToInteger(v));
  for (let prec = 1; prec <= 17; prec++) {
    const s = formatGeneral(v, prec, false).replace(/e([+-])0+(\d+)/, 'e$1$2');
    if (parseFloat(s) === v) return s;
  }
  return String(v);
}

function exactDecimal(value, decimals) {
  if (!Number.isFinite(value)) return '0';
  const buf = new ArrayBuffer(8);
  const f64 = new Float64Array(buf);
  const u64 = new BigUint64Array(buf);
  f64[0] = value;
  const bits = u64[0];
  const neg = (bits >> 63n) === 1n;
  const expBits = Number((bits >> 52n) & 0x7ffn);
  let mant = bits & ((1n << 52n) - 1n);
  let e;
  if (expBits === 0) e = -1074;             // subnormal / zero
  else { mant |= 1n << 52n; e = expBits - 1075; }
  // value = mant * 2^e. scaled = value * 10^decimals = mant * 2^(e+decimals) * 5^decimals
  let num, den = 1n;
  if (decimals >= 0) {
    num = mant * (5n ** BigInt(decimals));
    const shift = e + decimals;
    if (shift >= 0) num <<= BigInt(shift);
    else den <<= BigInt(-shift);
  } else {
    const D = -decimals;
    num = mant;
    const shift = e - D;
    if (shift >= 0) num <<= BigInt(shift);
    else den <<= BigInt(-shift);
    den *= 5n ** BigInt(D);
  }
  const q = num / den;
  const r = num % den;
  const twice = 2n * r;
  let rounded = q;
  if (twice > den) rounded = q + 1n;
  else if (twice === den && q % 2n === 1n) rounded = q + 1n; // tie -> even
  // glibc keeps the sign even when a negative value rounds to zero (-0.000)
  const sign = neg ? '-' : '';
  let abs = rounded < 0n ? -rounded : rounded;
  if (rounded === 0n && decimals < 0) return sign + '0';
  if (decimals < 0) {
    // rounded is value in units of 10^(-decimals)
    return sign + abs.toString() + '0'.repeat(-decimals);
  }
  let digits = abs.toString();
  if (digits.length <= decimals) digits = digits.padStart(decimals + 1, '0');
  if (decimals === 0) return sign + digits;
  const intPart = digits.slice(0, digits.length - decimals);
  const fracPart = digits.slice(digits.length - decimals);
  return sign + intPart + '.' + fracPart;
}

function formatFixed(v, prec) {
  return exactDecimal(v, prec);
}

// %e / %E: d.dddde±XX (exponent at least two digits, like C)
function formatExponent(v, prec, upper) {
  if (v === 0 || Object.is(v, -0)) {
    const s = (Object.is(v, -0) ? '-' : '') + '0.' + '0'.repeat(prec) + 'e+00';
    return upper ? s.toUpperCase() : s;
  }
  const neg = v < 0 || Object.is(v, -0);
  const av = Math.abs(v);
  let X = Math.floor(Math.log10(av));
  if (av / 10 ** X >= 10) X++;
  if (av / 10 ** X < 1) X--;
  let mant = av / 10 ** X;
  let body = exactDecimal(mant, prec);
  if (body.split('.')[0].length > 1) { // e.g. "10.000000" after rounding up
    X++;
    mant = av / 10 ** X;
    body = exactDecimal(mant, prec);
  }
  const expSign = X < 0 ? '-' : '+';
  const expStr = String(Math.abs(X)).padStart(2, '0');
  const out = (neg ? '-' : '') + body + 'e' + expSign + expStr;
  return upper ? out.toUpperCase() : out;
}

// %g / %G: precision significant digits; %e style when the exponent is
// < -4 or >= precision; trailing zeros stripped (matches C semantics).
function formatGeneral(v, prec, upper) {
  let p = prec === -1 ? 6 : prec;
  if (p === 0) p = 1;
  if (v === 0 || Object.is(v, -0)) {
    const s = (Object.is(v, -0) ? '-' : '') + '0';
    return upper ? s.toUpperCase() : s;
  }
  const neg = v < 0 || Object.is(v, -0);
  const av = Math.abs(v);
  let X = Math.floor(Math.log10(av));
  if (av / 10 ** X >= 10) X++;
  if (av / 10 ** X < 1) X--;
  let body;
  if (X < -4 || X >= p) {
    // e-style with p-1 fractional digits
    let mant = av / 10 ** X;
    body = exactDecimal(mant, p - 1);
    if (body.split('.')[0].length > 1) {
      X++;
      mant = av / 10 ** X;
      body = exactDecimal(mant, p - 1);
    }
    body = body.replace(/0+$/, '');
    if (body.endsWith('.')) body = body.slice(0, -1);
    const expSign = X < 0 ? '-' : '+';
    body += 'e' + expSign + String(Math.abs(X)).padStart(2, '0');
  } else {
    // f-style with p-1-X fractional digits
    const frac = Math.max(p - 1 - X, 0);
    body = exactDecimal(av, frac);
    body = body.replace(/0+$/, '');
    if (body.endsWith('.')) body = body.slice(0, -1);
  }
  const out = (neg ? '-' : '') + body;
  return upper ? out.toUpperCase() : out;
}

// Next scanf conversion spec in fmt starting at position `start`.
// Returns { ch, pos }; literal text and %% are skipped (ch === 0 at end).

// ---------------------------------------------------------------------------
// Phase 7: Stack Virtual Machine
// ---------------------------------------------------------------------------

function runVirtualMachine(chunk, sem, inputs) {
  const mem = new Float64Array(MEM_MAX);
  const stack = [];         // each entry: {n: number} or {s: stringIndex}
  const frames = [];        // {func, retPC, bp}
  const consoleParts = [];
  const steps = [];
  const runtimeDiags = [];
  let waitingForInput = false;
  let inputPrompt = '';
  let inputIdx = 0;
  let exitCode = 0;
  let halted = false;
  let truncated = false;

  const { code, funcPC, strings } = chunk;

  // frame metadata for variable snapshots
  const funcLocals = new Map(); // funcName -> [{name, offset, size}]
  for (const [name, f] of sem.functions) {
    const locals = [];
    for (const [lname, rec] of f.frame) {
      if (!rec.isParam) locals.push({ name: lname, offset: rec.offset, size: rec.size });
    }
    funcLocals.set(name, locals);
  }
  const globalList = [];
  for (const [name, rec] of sem.globals) {
    globalList.push({ name, offset: rec.offset, size: rec.size });
  }

  const push = (v) => {
    if (stack.length >= STACK_MAX) { runtimeError('Operand stack overflow'); return; }
    stack.push({ n: v });
  };
  const pushStr = (idx) => stack.push({ s: idx });
  const pop = () => (stack.length > 0 ? stack.pop() : (runtimeError('Operand stack underflow'), { n: 0 }));

  function runtimeError(msg, line) {
    runtimeDiags.push({ level: 'runtime', msg, line: line || 0, column: 0 });
    halted = true;
  }

  const fmtNumber = (v) => cFormatValue(v);

  // deterministic PRNG state (glibc-style LCG), identical in the C backend
  let randState = 1;

  // Built-in functions implemented by the VM itself. Returns true when the
  // call was handled (a result value is pushed for value-returning builtins).
  function callBuiltinFunction(instr) {
    const name = instr.symbol;
    const nargs = instr.operand;
    if (stack.length < nargs) { runtimeError(`Stack underflow in ${name}`, instr.line); return true; }
    const args = stack.splice(stack.length - nargs, nargs).map((c) => c.n || 0);

    switch (name) {
      case 'sqrt': push(Math.sqrt(args[0])); return true;
      case 'pow': push(Math.pow(args[0], args[1])); return true;
      case 'sin': push(Math.sin(args[0])); return true;
      case 'cos': push(Math.cos(args[0])); return true;
      case 'tan': push(Math.tan(args[0])); return true;
      case 'log': push(Math.log(args[0])); return true;
      case 'exp': push(Math.exp(args[0])); return true;
      case 'ceil': push(Math.ceil(args[0])); return true;
      case 'floor': push(Math.floor(args[0])); return true;
      case 'fabs': push(Math.abs(args[0])); return true;
      case 'abs': push(Math.abs(truncateToInteger(args[0]))); return true;
      case 'fmod': push(args[0] % args[1]); return true;
      case 'rand': {
        randState = (Math.imul(randState, 1103515245) + 12345) | 0;
        push((randState >>> 16) & 0x7fff);
        return true;
      }
      case 'srand': { randState = truncateToInteger(args[0]) | 0; push(0); return true; }
      case 'time': { push(1700000000); return true; } /* fixed constant for determinism */
      case 'exit': { exitCode = truncateToInteger(args[0]); halted = true; return true; }
      case 'assert': {
        if (truncateToInteger(args[0]) === 0) {
          runtimeError('Assertion failed', instr.line);
        }
        push(0);
        return true;
      }
      default:
        // not a builtin — restore args and report undefined
        for (let i = args.length - 1; i >= 0; i--) stack.push({ n: args[i] });
        return false;
    }
  }

  function recordStep(pc, instr) {
    if (steps.length >= TRACE_MAX_STEPS) { truncated = true; return; }
    const frame = frames[frames.length - 1];
    const variables = [];
    for (const g of globalList) variables.push({ name: g.name, value: mem[g.offset] });
    if (frame) {
      const locals = funcLocals.get(frame.func) || [];
      for (const l of locals) variables.push({ name: l.name, value: mem[frame.bp + l.offset] });
    }
    steps.push({
      step: steps.length,
      pc,
      line: instr.line,
      instruction: describeInstr(instr),
      stack: stack.map((e) => (e.s !== undefined ? 0 : e.n)),
      variables,
      frames: frames.map((f) => ({ func: f.func + '()', retAddr: '0x' + Math.max(0, f.retPC).toString(16).toUpperCase().padStart(4, '0') })),
      console: consoleParts.join('')
    });
  }

  function describeInstr(i) {
    switch (i.op) {
      case 'PUSH': return `PUSH ${fmtNumber(i.operand)}`;
      case 'LOAD': return `LOAD ${i.symbol}`;
      case 'STORE': return `STORE ${i.symbol}`;
      case 'ADDR': return `ADDR ${i.symbol}+${i.operand}`;
      case 'JMP': return `JMP ${i.symbol}`;
      case 'JZ': return `JZ ${i.symbol}`;
      case 'CALL': return `CALL ${i.symbol} (${i.operand} args)`;
      case 'RET': return 'RET';
      case 'PRINT': return 'PRINT';
      case 'INPUT': return 'INPUT';
      case 'HALT': return 'HALT';
      default: return i.op;
    }
  }

  const toUnsigned32 = (v) => {
    let t = truncateToInteger(v) % 4294967296;
    if (t < 0) t += 4294967296;
    return t;
  };

  // Exact decimal expansion of a finite double, correctly rounded to
  // `decimals` places with round-half-to-even — matching glibc printf (the
  // native backend uses snprintf, so this must agree byte-for-byte).
  // `decimals` may be negative (round to a multiple of 10^-decimals).
  function nextScanfConv(fmt, start) {
    let i = start;
    const n = fmt.length;
    while (i < n) {
      if (fmt[i] !== '%') { i++; continue; }
      i++;
      if (i < n && fmt[i] === '%') { i++; continue; }
      while (i < n && '-+ #0'.includes(fmt[i])) i++;
      while (i < n && /[0-9]/.test(fmt[i])) i++;
      if (i < n && fmt[i] === '.') {
        i++;
        while (i < n && /[0-9]/.test(fmt[i])) i++;
      }
      while (i < n && 'lhztj'.includes(fmt[i])) i++;
      if (i < n) return { ch: fmt[i], pos: i + 1 };
      return { ch: 0, pos: i };
    }
    return { ch: 0, pos: i };
  }

  function formatPrintf(fmt, args) {
    let out = '';
    let ai = 0;
    for (let i = 0; i < fmt.length; i++) {
      const ch = fmt[i];
      if (ch !== '%') { out += ch; continue; }
      i++;
      if (fmt[i] === '%') { out += '%'; continue; }
      // flags
      while (i < fmt.length && '-+ #0'.includes(fmt[i])) i++;
      // width (ignored for alignment in the educational VM, but consumed)
      while (i < fmt.length && /[0-9]/.test(fmt[i])) i++;
      let prec = -1;
      if (fmt[i] === '.') {
        i++;
        let digits = '';
        while (i < fmt.length && /[0-9]/.test(fmt[i])) digits += fmt[i++];
        prec = digits === '' ? 0 : parseInt(digits, 10);
      }
      while (fmt[i] === 'l' || fmt[i] === 'h' || fmt[i] === 'z' || fmt[i] === 'j' || fmt[i] === 't') i++;
      const conv = fmt[i];
      const arg = ai < args.length ? args[ai++] : { n: 0 };
      const num = arg.n || 0;
      if (conv === 'd' || conv === 'i') {
        out += String(truncateToInteger(num));
      } else if (conv === 'u') {
        out += String(toUnsigned32(num));
      } else if (conv === 'x') {
        out += toUnsigned32(num).toString(16);
      } else if (conv === 'X') {
        out += toUnsigned32(num).toString(16).toUpperCase();
      } else if (conv === 'o') {
        out += toUnsigned32(num).toString(8);
      } else if (conv === 'p') {
        out += '0x' + toUnsigned32(num).toString(16);
      } else if (conv === 'f') {
        out += formatFixed(num, prec === -1 ? 6 : prec);
      } else if (conv === 'e' || conv === 'E') {
        out += formatExponent(num, prec === -1 ? 6 : prec, conv === 'E');
      } else if (conv === 'g' || conv === 'G') {
        out += formatGeneral(num, prec === -1 ? 6 : prec, conv === 'G');
      } else if (conv === 'c') {
        out += String.fromCharCode(truncateToInteger(num) & 0xff);
      } else if (conv === 's') {
        // string pointer: read chars from memory until NUL
        let addr = truncateToInteger(num);
        let s = '';
        let guard = 0;
        while (addr >= 0 && addr < MEM_MAX && guard < 4096) {
          const ch = mem[addr];
          if (ch === 0) break;
          s += String.fromCharCode(ch);
          addr++; guard++;
        }
        out += s;
      } else {
        out += '%' + (conv || '');
      }
    }
    return out;
  }

  // ---- boot: intern string literals into memory, then call main ----
  const mainPC = funcPC.get('main');
  if (mainPC === undefined) {
    runtimeError("No 'main' entry point in bytecode");
    return finish();
  }

  // String literals live in memory (one slot per char + NUL) so they can be
  // stored in char* variables and read back by printf("%s", p).
  const strBase = [];
  let cursor = sem.globalSlotCount;
  for (let si = 0; si < strings.length; si++) {
    strBase[si] = cursor;
    const s = strings[si];
    for (let k = 0; k < s.length && cursor < MEM_MAX; k++) mem[cursor++] = s.charCodeAt(k);
    if (cursor < MEM_MAX) mem[cursor++] = 0;
  }

  const mainFrameSize = frameSizeOf('main');
  const mainBP = cursor;
  frames.push({ func: 'main', retPC: -1, bp: mainBP });
  let memTop = mainBP + mainFrameSize;
  let pc = mainPC;
  let stepsExecuted = 0;

  function frameSizeOf(funcName) {
    const f = sem.functions.get(funcName);
    const base = f ? f.frameSize : 0;
    const temps = chunk.tempsByFunc.get(funcName) || 0;
    return base + temps;
  }

  while (!halted && pc < code.length) {
    if (++stepsExecuted > VM_MAX_STEPS) {
      runtimeError('Execution step limit exceeded (possible infinite loop)');
      break;
    }
    const instr = code[pc];
    recordStep(pc, instr);
    if (halted) break;

    switch (instr.op) {
      case 'PUSH': push(instr.operand); pc++; break;
      case 'PUSH_STR': push(instr.operand < strBase.length ? strBase[instr.operand] : 0); pc++; break;
      case 'POP': if (stack.length) stack.pop(); pc++; break;
      case 'LOAD': {
        const addr = instr.isGlobal ? instr.slot : frames[frames.length - 1].bp + instr.slot;
        push(mem[addr]);
        pc++;
        break;
      }
      case 'STORE': {
        const v = pop();
        const addr = instr.isGlobal ? instr.slot : frames[frames.length - 1].bp + instr.slot;
        mem[addr] = v.n || 0;
        pc++;
        break;
      }
      case 'ADDR': {
        const base = instr.isGlobal ? instr.slot : frames[frames.length - 1].bp + instr.slot;
        push(base + instr.operand);
        pc++;
        break;
      }
      case 'IDX_ADDR': {
        const idxCell = pop();
        const idx = truncateToInteger(idxCell.n || 0);
        const base = instr.isGlobal ? instr.slot : frames[frames.length - 1].bp + instr.slot;
        if (instr.operand >= 0 && (idx < 0 || idx >= instr.operand)) {
          runtimeError(`Array index ${idx} out of bounds (size ${instr.operand})`, instr.line);
          break;
        }
        push(base + idx);
        pc++;
        break;
      }
      case 'LOAD_AT': {
        const addrCell = pop();
        const addr = truncateToInteger(addrCell.n || 0);
        if (addr < 0 || addr >= memTop) { runtimeError(`Invalid memory read at address ${addr}`, instr.line); break; }
        push(mem[addr]);
        pc++;
        break;
      }
      case 'STORE_AT': {
        const addrCell = pop();
        const valCell = pop();
        const addr = truncateToInteger(addrCell.n || 0);
        if (addr < 0 || addr >= memTop) { runtimeError(`Invalid memory write at address ${addr}`, instr.line); break; }
        mem[addr] = valCell.n || 0;
        pc++;
        break;
      }
      case 'ADD': { const b = pop(), a = pop(); push((a.n || 0) + (b.n || 0)); pc++; break; }
      case 'SUB': { const b = pop(), a = pop(); push((a.n || 0) - (b.n || 0)); pc++; break; }
      case 'MUL': { const b = pop(), a = pop(); push((a.n || 0) * (b.n || 0)); pc++; break; }
      case 'DIV': {
        const b = pop(), a = pop();
        const bi = truncateToInteger(b.n || 0);
        if (bi === 0) { runtimeError('Division by zero', instr.line); break; }
        push(Math.trunc(truncateToInteger(a.n || 0) / bi));
        pc++;
        break;
      }
      case 'DIVF': {
        const b = pop(), a = pop();
        if ((b.n || 0) === 0) { runtimeError('Division by zero', instr.line); break; }
        push((a.n || 0) / (b.n || 0));
        pc++;
        break;
      }
      case 'MOD': {
        const b = pop(), a = pop();
        const bi = truncateToInteger(b.n || 0);
        if (bi === 0) { runtimeError('Division by zero (modulo)', instr.line); break; }
        push(truncateToInteger(a.n || 0) % bi);
        pc++;
        break;
      }
      case 'NEG': { const a = pop(); push(-(a.n || 0)); pc++; break; }
      case 'NOT': { const a = pop(); push((a.n || 0) === 0 ? 1 : 0); pc++; break; }
      case 'EQ': { const b = pop(), a = pop(); push((a.n || 0) === (b.n || 0) ? 1 : 0); pc++; break; }
      case 'NEQ': { const b = pop(), a = pop(); push((a.n || 0) !== (b.n || 0) ? 1 : 0); pc++; break; }
      case 'LT': { const b = pop(), a = pop(); push((a.n || 0) < (b.n || 0) ? 1 : 0); pc++; break; }
      case 'GT': { const b = pop(), a = pop(); push((a.n || 0) > (b.n || 0) ? 1 : 0); pc++; break; }
      case 'LEQ': { const b = pop(), a = pop(); push((a.n || 0) <= (b.n || 0) ? 1 : 0); pc++; break; }
      case 'GEQ': { const b = pop(), a = pop(); push((a.n || 0) >= (b.n || 0) ? 1 : 0); pc++; break; }
      case 'AND': { const b = pop(), a = pop(); push((a.n || 0) !== 0 && (b.n || 0) !== 0 ? 1 : 0); pc++; break; }
      case 'OR': { const b = pop(), a = pop(); push((a.n || 0) !== 0 || (b.n || 0) !== 0 ? 1 : 0); pc++; break; }
      // bitwise ops use int32 semantics (identical in the JS and C VMs)
      case 'BAND': { const b = pop(), a = pop(); push(truncateToInteger(a.n || 0) & truncateToInteger(b.n || 0)); pc++; break; }
      case 'BOR': { const b = pop(), a = pop(); push(truncateToInteger(a.n || 0) | truncateToInteger(b.n || 0)); pc++; break; }
      case 'BXOR': { const b = pop(), a = pop(); push(truncateToInteger(a.n || 0) ^ truncateToInteger(b.n || 0)); pc++; break; }
      case 'BNOT': { const a = pop(); push(~truncateToInteger(a.n || 0)); pc++; break; }
      case 'SHL': { const b = pop(), a = pop(); push(truncateToInteger(a.n || 0) << (truncateToInteger(b.n || 0) & 31)); pc++; break; }
      case 'SHR': { const b = pop(), a = pop(); push(truncateToInteger(a.n || 0) >> (truncateToInteger(b.n || 0) & 31)); pc++; break; }
      case 'CVT_I': { const a = pop(); push(truncateToInteger(a.n || 0)); pc++; break; }
      case 'CVT_F': { const a = pop(); push(a.n || 0); pc++; break; }
      case 'JMP': pc = instr.operand; break;
      case 'JZ': { const v = pop(); pc = (v.n || 0) === 0 ? instr.operand : pc + 1; break; }
      case 'CALL': {
        const target = funcPC.get(instr.symbol);
        if (target === undefined) {
          if (callBuiltinFunction(instr)) { pc++; break; }
          runtimeError(`Call to undefined function '${instr.symbol}'`, instr.line);
          break;
        }
        if (frames.length >= CALL_DEPTH_MAX) { runtimeError('Call stack overflow (recursion too deep)', instr.line); break; }
        const nargs = instr.operand;
        if (stack.length < nargs) { runtimeError('Stack underflow in CALL', instr.line); break; }
        const bp = memTop;
        const fsize = frameSizeOf(instr.symbol);
        if (bp + fsize >= MEM_MAX) { runtimeError('Memory exhausted (too many locals/frames)', instr.line); break; }
        for (let i = 0; i < nargs; i++) {
          const cell = stack[stack.length - nargs + i];
          mem[bp + i] = cell.n || 0;
        }
        stack.length -= nargs;
        frames.push({ func: instr.symbol, retPC: pc + 1, bp });
        memTop = bp + fsize;
        pc = target;
        break;
      }
      case 'RET': {
        const hasVal = instr.operand === 1;
        const retVal = hasVal ? pop() : { n: 0 };
        const frame = frames.pop();
        if (!frame) { runtimeError('RET with empty call stack', instr.line); break; }
        memTop = frame.bp;
        if (frames.length === 0) {
          exitCode = truncateToInteger(retVal.n || 0);
          halted = true;
          break;
        }
        // Always push exactly one value so callers (STORE/POP) stay balanced.
        push(retVal.n || 0);
        pc = frame.retPC;
        break;
      }
      case 'PRINT': {
        const nargs = instr.operand;
        if (stack.length < nargs) { runtimeError('Stack underflow in PRINT', instr.line); break; }
        const args = stack.splice(stack.length - nargs, nargs);
        const fmt = strings[instr.fmtIdx] || '';
        consoleParts.push(formatPrintf(fmt, args));
        pc++;
        break;
      }
      case 'INPUT': {
        const nTargets = instr.operand;
        if (stack.length < nTargets) { runtimeError('Stack underflow in INPUT', instr.line); break; }
        if (inputIdx + nTargets > inputs.length) {
          waitingForInput = true;
          inputPrompt = `Enter ${nTargets} value(s) for scanf (${strings[instr.fmtIdx] || '%d'})`;
          halted = true;
          break;
        }
        const addrs = stack.splice(stack.length - nTargets, nTargets);
        const fmt = strings[instr.fmtIdx] || '%d';
        let fpos = 0;
        for (let i = 0; i < nTargets; i++) {
          const conv = nextScanfConv(fmt, fpos);
          fpos = conv.pos;
          const raw = String(inputs[inputIdx++]).trim();
          const addr = truncateToInteger(addrs[i].n || 0);
          if (addr < 0 || addr >= memTop) { runtimeError(`Invalid scanf target address ${addr}`, instr.line); break; }
          if (conv.ch === 'c') {
            // %c reads the first character of the next input line
            mem[addr] = raw ? raw.charCodeAt(0) : 0;
          } else if (conv.ch === 's') {
            // %s copies the input token (the line, trimmed) plus NUL
            let cap = memTop - addr;
            if (cap > 4096) cap = 4096;
            if (cap < 1) cap = 1;
            let k = 0;
            while (k < raw.length && k < cap - 1) { mem[addr + k] = raw.charCodeAt(k); k++; }
            mem[addr + k] = 0;
          } else {
            let val = parseFloat(raw);
            if (Number.isNaN(val)) val = 0;
            if (['d', 'i', 'u', 'x', 'X', 'o'].includes(conv.ch)) val = truncateToInteger(val);
            mem[addr] = val;
          }
        }
        pc++;
        break;
      }
      case 'HALT': halted = true; break;
      default: pc++; break;
    }
  }

  function finish() {
    return {
      steps,
      truncated,
      consoleOutput: consoleParts.join(''),
      waitingForInput,
      inputPrompt,
      runtimeDiags,
      exitCode
    };
  }

  return finish();
}

// ---------------------------------------------------------------------------
// Pipeline entry point
// ---------------------------------------------------------------------------

export function compileCSource(sourceCode, userInputs = []) {
  const startTime = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const diags = new DiagList();

  // Phase 1: Lexical analysis
  const tokens = tokenize(sourceCode, diags);

  // Phase 2: Parsing
  const ast = parseProgram(tokens, diags);

  // Phase 3: Semantic analysis + layout
  const sem = analyzeSemantics(ast, diags);

  const compileErrors = diags.hasErrors();

  let tacList = [];
  let optTac = [];
  let metrics = { constant_fold: 0, constant_prop: 0, dead_code: 0, strength_reduce: 0, reduction_percentage: 0 };
  let bytecode = [];
  let vmTrace = [];
  let vmTruncated = false;
  let consoleOutput = '';
  let waitingForInput = false;
  let inputPrompt = '';
  let exitCode = 0;

  if (!compileErrors) {
    // Phase 4: TAC generation
    const tacResult = generateTAC(ast, sem, diags);
    tacList = tacResult.instrs;

    // Phase 5: Optimization
    const optResult = optimizeTAC(tacList, tacResult.tempTypes);
    optTac = optResult.optTac;
    metrics = optResult.metrics;

    // Phase 6: Bytecode generation
    const chunk = generateBytecode(optTac, sem, tacResult.tempTypes, tacResult.strings);
    bytecode = chunk.code.map((c) => ({ pc: c.pc, op: c.op, operand: c.operand, symbol: c.symbol, line: c.line }));

    // Phase 7: VM execution
    if (!diags.hasErrors()) {
      const vm = runVirtualMachine(chunk, sem, userInputs);
      vmTrace = vm.steps;
      vmTruncated = vm.truncated;
      consoleOutput = vm.consoleOutput;
      waitingForInput = vm.waitingForInput;
      inputPrompt = vm.inputPrompt;
      exitCode = vm.exitCode;
      vm.runtimeDiags.forEach((d) => diags.add(d.level, d.line, d.column, d.msg));
    }
  }

  const endTime = typeof performance !== 'undefined' ? performance.now() : Date.now();

  const serializedTokens = tokens.map((t) => ({ type: t.type, lexeme: t.lexeme, line: t.line, column: t.column }));

  return {
    success: !diags.hasErrors(),
    engine: 'browser-js',
    compile_time_ms: Math.round((endTime - startTime) * 100) / 100,
    tokens: serializedTokens,
    ast,
    symbolTable: sem.symbols,
    tac: tacList,
    optTac,
    metrics,
    bytecode,
    vmTrace,
    vmTraceTruncated: vmTruncated,
    waitingForInput,
    inputPrompt,
    consoleOutput,
    exitCode,
    diagnostics: diags.items
  };
}
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
// Diagnostics
// ---------------------------------------------------------------------------

class DiagList {
  constructor() { this.items = []; }
  add(level, line, column, msg) { this.items.push({ level, msg, line, column }); }
  hasErrors() { return this.items.some((d) => d.level === 'error'); }
}

// ---------------------------------------------------------------------------
// Phase 1: Lexer
// ---------------------------------------------------------------------------

function tokenize(source, diags) {
  const tokens = [];
  let pos = 0, line = 1, col = 1;
  const n = source.length;

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
      let word = '#';
      while (pos < n && /[a-zA-Z]/.test(peek())) word += advance();
      if (word === '#include') push('TOKEN_INCLUDE', word, startLine, startCol);
      else if (word === '#define') push('TOKEN_DEFINE', word, startLine, startCol);
      else push('TOKEN_HASH', word, startLine, startCol);
      continue;
    }

    // Identifiers / keywords
    if (/[a-zA-Z_]/.test(c)) {
      let lex = '';
      while (pos < n && /[a-zA-Z0-9_]/.test(peek())) lex += advance();
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
      if (peek() === 'f' || peek() === 'F' || peek() === 'l' || peek() === 'L') advance(); // suffix
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

    // Multi-character operators
    const two = c + peek(1);
    const TWO_CHAR = {
      '++': 'TOKEN_PLUS_PLUS', '--': 'TOKEN_MINUS_MINUS', '==': 'TOKEN_EQ', '!=': 'TOKEN_NEQ',
      '<=': 'TOKEN_LEQ', '>=': 'TOKEN_GEQ', '&&': 'TOKEN_AND', '||': 'TOKEN_OR',
      '+=': 'TOKEN_PLUS_ASSIGN', '-=': 'TOKEN_MINUS_ASSIGN', '*=': 'TOKEN_STAR_ASSIGN',
      '/=': 'TOKEN_SLASH_ASSIGN', '%=': 'TOKEN_PERCENT_ASSIGN', '->': 'TOKEN_ARROW',
      '<<': 'TOKEN_LSHIFT', '>>': 'TOKEN_RSHIFT'
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
    const left = parseLogicalOr();
    const t = peek();
    const ASSIGN_OPS = {
      TOKEN_ASSIGN: '=', TOKEN_PLUS_ASSIGN: '+=', TOKEN_MINUS_ASSIGN: '-=',
      TOKEN_STAR_ASSIGN: '*=', TOKEN_SLASH_ASSIGN: '/=', TOKEN_PERCENT_ASSIGN: '%='
    };
    if (ASSIGN_OPS[t.type]) {
      advanceTok();
      const right = parseAssignment(); // right-associative
      const node = makeNode(ASSIGN_OPS[t.type] === '=' ? 'NODE_ASSIGNMENT' : 'NODE_COMPOUND_ASSIGN', t.line);
      node.op = ASSIGN_OPS[t.type];
      node.children.push(left, right);
      return node;
    }
    return left;
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

  const parseLogicalOr = () => binaryLevel(parseLogicalAnd, { TOKEN_OR: '||' });
  const parseLogicalAnd = () => binaryLevel(parseEquality, { TOKEN_AND: '&&' });
  const parseEquality = () => binaryLevel(parseRelational, { TOKEN_EQ: '==', TOKEN_NEQ: '!=' });
  const parseRelational = () => binaryLevel(parseAdditive, { TOKEN_LT: '<', TOKEN_GT: '>', TOKEN_LEQ: '<=', TOKEN_GEQ: '>=' });
  const parseAdditive = () => binaryLevel(parseMultiplicative, { TOKEN_PLUS: '+', TOKEN_MINUS: '-' });
  const parseMultiplicative = () => binaryLevel(parseUnary, { TOKEN_STAR: '*', TOKEN_SLASH: '/', TOKEN_PERCENT: '%' });

  function parseUnary() {
    const t = peek();
    if (t.type === 'TOKEN_MINUS' || t.type === 'TOKEN_NOT' || t.type === 'TOKEN_STAR' ||
        t.type === 'TOKEN_AMPERSAND' || t.type === 'TOKEN_PLUS_PLUS' || t.type === 'TOKEN_MINUS_MINUS') {
      advanceTok();
      const node = makeNode('NODE_UNARY_OP', t.line);
      node.op = { TOKEN_MINUS: '-', TOKEN_NOT: '!', TOKEN_STAR: '*', TOKEN_AMPERSAND: '&',
                  TOKEN_PLUS_PLUS: '++', TOKEN_MINUS_MINUS: '--' }[t.type];
      node.children.push(parseUnary());
      return node;
    }
    return parsePostfix();
  }

  function parsePostfix() {
    let expr = parsePrimary();
    for (;;) {
      const t = peek();
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
      advanceTok();
      const node = makeNode('NODE_STRING_LITERAL', t.line);
      node.string_val = t.stringValue;
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
        decl.children.push((() => { const nn = makeNode('NODE_INT_LITERAL', sz.line); nn.num_val = parseInt(sz.lexeme, 16); return nn; })());
        decl.has_size = true;
      }
      expect('TOKEN_RBRACKET', "']' after array size");
    }
    if (match('TOKEN_ASSIGN')) {
      if (decl.is_array && check('TOKEN_LBRACE')) {
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

  function parseStatement(inLoop) {
    const t = peek();

    if (t.type === 'TOKEN_IF') {
      advanceTok();
      const node = makeNode('NODE_IF_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'if'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after condition");
      node.children.push(parseStatement(inLoop));
      if (match('TOKEN_ELSE')) node.children.push(parseStatement(inLoop));
      return node;
    }
    if (t.type === 'TOKEN_WHILE') {
      advanceTok();
      const node = makeNode('NODE_WHILE_STMT', t.line);
      expect('TOKEN_LPAREN', "'(' after 'while'");
      node.children.push(parseExpression());
      expect('TOKEN_RPAREN', "')' after condition");
      node.children.push(parseStatement(true));
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
      } else if (isTypeToken()) {
        const typeTok = advanceTok();
        node.children.push(parseVarDeclTail(TYPE_NAMES[typeTok.type], typeTok.line));
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
      node.children.push(parseStatement(true));
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
        comp.children.push(parseStatement(inLoop));
      }
      expect('TOKEN_RBRACE', "'}' to close block");
      return comp;
    }
    if (isTypeToken()) {
      const typeTok = advanceTok();
      return parseVarDeclTail(TYPE_NAMES[typeTok.type], typeTok.line);
    }
    if (t.type === 'TOKEN_STRUCT') {
      // struct-typed local variable: struct Name x;
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
      if (!isTypeToken()) {
        diags.add('error', peek().line, peek().column, `Expected parameter type but found '${peek().lexeme}'`);
        break;
      }
      const typeTok = advanceTok();
      let typeName = TYPE_NAMES[typeTok.type];
      let isPointer = false;
      while (check('TOKEN_STAR')) { advanceTok(); isPointer = true; }
      const id = expect('TOKEN_IDENTIFIER', 'parameter name');
      const param = makeNode('NODE_PARAMETER', typeTok.line);
      param.type_name = isPointer ? typeName + '*' : typeName;
      param.identifier = id ? id.lexeme : '<error>';
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

    if (t.type === 'TOKEN_STRUCT') {
      advanceTok();
      const nameTok = expect('TOKEN_IDENTIFIER', 'struct name');
      if (check('TOKEN_LBRACE')) {
        advanceTok();
        const def = makeNode('NODE_STRUCT_DEF', t.line);
        def.identifier = nameTok ? nameTok.lexeme : '<error>';
        while (!check('TOKEN_RBRACE') && !check('TOKEN_EOF')) {
          if (!isTypeToken()) {
            diags.add('error', peek().line, peek().column, `Expected field type in struct but found '${peek().lexeme}'`);
            skipStatement();
            continue;
          }
          const ft = advanceTok();
          const field = makeNode('NODE_STRUCT_FIELD', ft.line);
          field.type_name = TYPE_NAMES[ft.type];
          const fid = expect('TOKEN_IDENTIFIER', 'field name');
          field.identifier = fid ? fid.lexeme : '<error>';
          if (match('TOKEN_LBRACKET')) {
            field.is_array = true;
            if (check('TOKEN_INTEGER_LITERAL')) {
              const sz = advanceTok();
              field.children.push((() => { const nn = makeNode('NODE_INT_LITERAL', sz.line); nn.num_val = parseInt(sz.lexeme, 16); return nn; })());
              field.has_size = true;
            }
            expect('TOKEN_RBRACKET', "']' after field size");
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

    if (isTypeToken()) {
      const typeTok = advanceTok();
      let typeName = TYPE_NAMES[typeTok.type];
      while (check('TOKEN_STAR')) { advanceTok(); typeName += '*'; }
      const id = expect('TOKEN_IDENTIFIER', 'function or variable name');
      if (!id) { skipStatement(); continue; }
      if (check('TOKEN_LPAREN')) {
        advanceTok();
        const func = makeNode('NODE_FUNCTION_DEF', typeTok.line);
        func.type_name = typeName;
        func.identifier = id.lexeme;
        parseParameterList(func);
        expect('TOKEN_RPAREN', "')' after parameters");
        if (!check('TOKEN_LBRACE')) {
          diags.add('error', peek().line, peek().column, "Expected '{' after function signature");
          skipStatement();
          root.children.push(func);
          continue;
        }
        func.children.push(parseStatement(false)); // body
        root.children.push(func);
      } else {
        // global variable(s), possibly comma-separated
        const first = parseSingleDeclarator(typeName, typeTok.line, id);
        if (!check('TOKEN_COMMA')) {
          expect('TOKEN_SEMICOLON', "';' after declaration");
          root.children.push(first);
        } else {
          const group = makeNode('NODE_DECL_LIST', typeTok.line);
          group.children.push(first);
          while (match('TOKEN_COMMA')) {
            group.children.push(parseSingleDeclarator(typeName, typeTok.line, null));
          }
          expect('TOKEN_SEMICOLON', "';' after declaration");
          root.children.push(group);
        }
      }
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

function analyzeSemantics(ast, diags) {
  const symbols = [];            // serialized symbol table (insertion order)
  const globals = new Map();     // name -> symbol record
  const functions = new Map();   // name -> {node, returnType, params:[{name,type}], frame:Map}
  const structs = new Map();     // name -> {fields:Map(name->{type,offset,size,is_array}), size}
  const globalSlots = { next: 0 };

  // built-ins first (stable order for the symbol table UI)
  symbols.push({ scope: 'global', name: 'printf', kind: 'Function', type: 'int', address: '0x0000', params: -1 });
  symbols.push({ scope: 'global', name: 'scanf', kind: 'Function', type: 'int', address: '0x0000', params: -1 });

  const baseTypeOf = (typeName) => typeName.replace(/\*/g, '').trim();
  const isPointerType = (typeName) => typeName.endsWith('*');
  const typeSize = (typeName) => {
    if (typeName.startsWith('struct ')) {
      const s = structs.get(typeName.slice(7));
      return s ? s.size : 1;
    }
    return 1;
  };

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
        const size = f.is_array ? (f.has_size ? Math.max(1, truncateToInteger(f.children[0].num_val)) : 1) : 1;
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
        diags.add('error', node.line, 1, `Redefinition of function '${node.identifier}'`);
        continue;
      }
      const params = node.children.filter((c) => c.type === 'NODE_PARAMETER')
        .map((p) => ({ name: p.identifier, type: p.type_name }));
      functions.set(node.identifier, { node, returnType: node.type_name, params, frame: new Map(), frameSize: 0 });
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
        ? (node.has_size ? Math.max(1, truncateToInteger(node.children[0].num_val)) : Math.max(1, node.children.length))
        : typeSize(node.type_name);
      const rec = {
        name: node.identifier, type: node.type_name, is_array: !!node.is_array,
        size, isGlobal: true, offset: globalSlots.next, isTemp: false, isParam: false
      };
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

    const declare = (name, type, node, isParam) => {
      const scope = scopeStack[scopeStack.length - 1];
      if (scope.has(name)) {
        diags.add('error', node.line, 1, `Duplicate declaration of '${name}'`);
        return;
      }
      const size = node.is_array
        ? (node.has_size ? Math.max(1, truncateToInteger(node.children[0].num_val)) : Math.max(1, node.children.length))
        : typeSize(type);
      const rec = { name, type, is_array: !!node.is_array, size, isGlobal: false, offset: nextSlot, isTemp: false, isParam };
      nextSlot += size;
      scope.set(name, rec);
      frame.set(name, rec);
      symbols.push({
        scope: fname, name, kind: node.is_array ? 'Array' : (isParam ? 'Parameter' : 'Variable'),
        type, address: '0x' + (rec.offset * 4).toString(16).toUpperCase().padStart(4, '0'), params: 0
      });
    };

    for (const p of frec.params) {
      declare(p.name, p.type, { line: frec.node.line, is_array: false, children: [] }, true);
    }

    const lookup = (name) => {
      for (let i = scopeStack.length - 1; i >= 0; i--) {
        if (scopeStack[i].has(name)) return scopeStack[i].get(name);
      }
      return null;
    };

    const resolve = (name, line) => {
      const rec = lookup(name);
      if (rec) return rec;
      if (globals.has(name)) return globals.get(name);
      if (functions.has(name)) return { name, isFunction: true };
      if (name === 'printf' || name === 'scanf') return { name, isBuiltin: true };
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
          if (node.identifier === 'printf' || node.identifier === 'scanf') return 'int';
          const f = functions.get(node.identifier);
          if (!f) {
            if (!resolve(node.identifier, node.line).isFunction) {
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

    const walkStmt = (node, inLoop) => {
      if (!node) return;
      switch (node.type) {
        case 'NODE_DECL_LIST':
          (node.children || []).forEach((c) => walkStmt(c, inLoop));
          return;
        case 'NODE_VAR_DECL':
          declare(node.identifier, node.type_name, node, false);
          if (node.is_array) (node.has_size ? node.children.slice(1) : node.children).forEach(walkExpr);
          else (node.children || []).forEach(walkExpr);
          return;
        case 'NODE_COMPOUND_STMT':
          scopeStack.push(new Map());
          (node.children || []).forEach((c) => walkStmt(c, inLoop));
          scopeStack.pop();
          return;
        case 'NODE_IF_STMT':
          walkExpr(node.children[0]);
          walkStmt(node.children[1], inLoop);
          if (node.children[2]) walkStmt(node.children[2], inLoop);
          return;
        case 'NODE_WHILE_STMT':
          walkExpr(node.children[0]);
          walkStmt(node.children[1], true);
          return;
        case 'NODE_FOR_STMT':
          scopeStack.push(new Map());
          walkStmt(node.children[0], true);
          walkExpr(node.children[1]);
          walkExpr(node.children[2]);
          walkStmt(node.children[3], true);
          scopeStack.pop();
          return;
        case 'NODE_BREAK_STMT':
        case 'NODE_CONTINUE_STMT':
          if (!inLoop) {
            diags.add('error', node.line, 1,
              `'${node.type === 'NODE_BREAK_STMT' ? 'break' : 'continue'}' used outside of a loop`);
          }
          return;
        case 'NODE_RETURN_STMT':
          (node.children || []).forEach(walkExpr);
          return;
        case 'NODE_EXPRESSION_STMT':
          (node.children || []).forEach(walkExpr);
          return;
        default:
          (node.children || []).forEach((c) => walkStmt(c, inLoop));
      }
    };

    const body = frec.node.children.find((c) => c.type !== 'NODE_PARAMETER');
    walkStmt(body, false);
    frec.frameSize = nextSlot;
  }

  return { symbols, globals, functions, structs, globalSlotCount: globalSlots.next };
}

// ---------------------------------------------------------------------------
// Phase 4: Three-Address Code generation
// ---------------------------------------------------------------------------

function generateTAC(ast, sem, diags) {
  const instrs = [];
  let tempCount = 0;
  let labelCount = 0;
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
      return isIntegral(v) && Math.abs(v) < 1e15 ? String(Math.trunc(v)) + '.0' : String(v);
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
      return { place: node.identifier, type: semExprType(node), isConst: false };
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
      const resType = ['==', '!=', '<', '>', '<=', '>='].includes(op)
        ? 'int'
        : (isFloatType(l.type) || isFloatType(r.type)) ? 'double' : 'int';
      const t = newTemp(resType);
      emit(op, t, l.place, r.place, node.line);
      return { place: t, type: resType, isConst: false };
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
      if (node.op === '&') {
        const inner = node.children[0];
        if (inner.type === 'NODE_IDENTIFIER') {
          const t = newTemp('ptr');
          emit('ADDR', t, inner.identifier, '0', inner.line);
          return { place: t, type: 'ptr', isConst: false };
        }
        if (inner.type === 'NODE_INDEX') {
          const addr = genIndexAddr(inner);
          return { place: addr, type: 'ptr', isConst: false };
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
      const t = newTemp('int');
      emit('LOAD_PTR', t, addr, '', node.line);
      return { place: t, type: 'int', isConst: false };
    }

    if (node.type === 'NODE_MEMBER') {
      const addr = genMemberAddr(node);
      if (!addr) return { place: '0', type: 'int', isConst: true };
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
    if (sem.globals.has(name)) return sem.globals.get(name);
    for (const [, f] of sem.functions) {
      if (f.frame.has(name)) return f.frame.get(name);
    }
    return null;
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
    const baseName = base.type === 'NODE_IDENTIFIER' ? base.identifier : '0';
    emit('IDX_ADDR', t, baseName, idx.place, node.line);
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
          emit('ADDR', t, base.identifier, String(f.offset), node.line);
          return { place: t, fieldType: f.is_array ? f.type + '*' : f.type };
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
    // user function: push args left-to-right
    for (const arg of node.children) {
      const a = genExpr(arg);
      emit('PARAM', '', a.place, '', node.line);
    }
    const t = newTemp('int');
    emit('CALL', t, node.identifier, String(node.children.length), node.line);
    return { place: t, type: 'int', isConst: false };
  }

  // Compute address place for an lvalue; returns {mode:'direct',name} or {mode:'addr',place}
  function lvalueAddr(node) {
    if (node.type === 'NODE_IDENTIFIER') {
      return { mode: 'direct', name: node.identifier, line: node.line };
    }
    if (node.type === 'NODE_INDEX') {
      return { mode: 'addr', place: genIndexAddr(node), line: node.line };
    }
    if (node.type === 'NODE_MEMBER') {
      const addr = genMemberAddr(node);
      return addr ? { mode: 'addr', place: addr.place, line: node.line } : null;
    }
    if (node.type === 'NODE_UNARY_OP' && node.op === '*') {
      const p = genExpr(node.children[0]);
      return { mode: 'addr', place: p.place, line: node.line };
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
    const opMap = { '+=': '+', '-=': '-', '*=': '*', '/=': '/', '%=': '%' };
    const op = opMap[node.op];
    const lv = lvalueAddr(target);
    if (!lv) return { place: '0', type: 'int' };
    const oldV = lv.mode === 'direct'
      ? { place: lv.name, type: semExprType(target) }
      : (() => { const t = newTemp('int'); emit('LOAD_PTR', t, lv.place, '', node.line); return { place: t, type: 'int' }; })();
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
      : (() => { const t = newTemp('int'); emit('LOAD_PTR', t, lv.place, '', node.line); return { place: t, type: 'int' }; })();
    const t = newTemp(oldV.type === 'double' ? 'double' : 'int');
    emit(op, t, oldV.place, '1', node.line);
    storeToLvalue(lv, t, node.line);
    if (prefix) return { place: t, type: oldV.type === 'double' ? 'double' : 'int', isConst: false };
    return oldV;
  }

  // ---- statements ----
  const loopStack = [];

  function genVarDecl(node) {
    if (node.is_array) {
      // children[0] = size literal when has_size, rest = initializer values
      const inits = node.has_size ? node.children.slice(1) : node.children.slice(0);
      inits.forEach((initExpr, i) => {
        const v = genExpr(initExpr);
        const idxT = newTemp('ptr');
        emit('IDX_ADDR', idxT, node.identifier, String(i), node.line);
        emit('STORE_PTR', '', idxT, v.place, node.line);
      });
      return;
    }
    if (node.children.length > 0) {
      const v = genExpr(node.children[0]);
      emit('=', node.identifier, v.place, '', node.line);
    }
  }

  function genStmt(node) {
    if (!node || node.type === 'NODE_EMPTY' || node.type === 'NODE_ERROR') return;

    if (node.type === 'NODE_DECL_LIST') { node.children.forEach(genStmt); return; }

    if (node.type === 'NODE_VAR_DECL') { genVarDecl(node); return; }

    if (node.type === 'NODE_COMPOUND_STMT') {
      node.children.forEach(genStmt);
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
      loopStack.push({ brk: lEnd, cont: lStart });
      emit('LABEL', lStart, '', '', node.line);
      const cond = genExpr(node.children[0]);
      emit('IF_FALSE', lEnd, cond.place, '', node.line);
      genStmt(node.children[1]);
      emit('GOTO', lStart, '', '', node.line);
      emit('LABEL', lEnd, '', '', node.line);
      loopStack.pop();
      return;
    }

    if (node.type === 'NODE_FOR_STMT') {
      const [init, cond, incr, body] = node.children;
      const lStart = newLabel();
      const lStep = newLabel();
      const lEnd = newLabel();
      genStmt(init);
      loopStack.push({ brk: lEnd, cont: lStep });
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
      return;
    }

    if (node.type === 'NODE_BREAK_STMT') {
      const loop = loopStack[loopStack.length - 1];
      if (loop) emit('GOTO', loop.brk, '', '', node.line);
      return;
    }
    if (node.type === 'NODE_CONTINUE_STMT') {
      const loop = loopStack[loopStack.length - 1];
      if (loop) emit('GOTO', loop.cont, '', '', node.line);
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
  function emitGlobalInits() {
    for (const top of ast.children) {
      const decls = top.type === 'NODE_DECL_LIST' ? top.children : [top];
      for (const d of decls) {
        if (!d || d.type !== 'NODE_VAR_DECL') continue;
        if (d.is_array) {
          genVarDecl(d);
        } else if (d.children.length > 0) {
          const v = genExpr(d.children[0]);
          emit('=', d.identifier, v.place, '', d.line);
        }
      }
    }
  }

  for (const top of ast.children) {
    if (top.type !== 'NODE_FUNCTION_DEF') continue;
    emit('FUNC_BEGIN', top.identifier, '', '', top.line);
    if (top.identifier === 'main') emitGlobalInits();
    const body = top.children.find((c) => c.type !== 'NODE_PARAMETER');
    genStmt(body);
    // implicit return when control reaches the end of the body
    const last = instrs[instrs.length - 1];
    if (!last || last.op !== 'RETURN') {
      emit('RETURN', '', top.identifier === 'main' ? '0' : '', '', top.line);
    }
    emit('FUNC_END', top.identifier, '', '', top.line);
  }

  return { instrs, tempTypes, strings, tempCount, labelCount };
}

// ---------------------------------------------------------------------------
// Phase 5: Optimizer (constant folding, constant propagation, strength
// reduction, dead code elimination) — real rewrites with real metrics.
// ---------------------------------------------------------------------------

const BIN_OPS = new Set(['+', '-', '*', '/', '%', '==', '!=', '<', '>', '<=', '>=']);

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
    const pureOps = new Set(['=', '+', '-', '*', '/', '%', 'neg', '!', '==', '!=', '<', '>', '<=', '>=']);
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
      if (f.frame.has(place)) {
        const r = f.frame.get(place);
        return { isGlobal: false, slot: r.offset, size: r.size };
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

  const isNumericPlace = (p) => /^-?\d+$/.test(p) || /^-?\d+\.\d+$/.test(p);

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
      case '&&': case '||': {
        pushPlace(ins.a1, ins.line);
        pushPlace(ins.a2, ins.line);
        let op = { '+': 'ADD', '-': 'SUB', '*': 'MUL', '%': 'MOD', '==': 'EQ', '!=': 'NEQ',
                   '<': 'LT', '>': 'GT', '<=': 'LEQ', '>=': 'GEQ', '&&': 'AND', '||': 'OR' }[ins.op];
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

  const fmtNumber = (v) => (isIntegral(v) ? String(truncateToInteger(v)) : String(v));

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

  function formatPrintf(fmt, args) {
    let out = '';
    let ai = 0;
    for (let i = 0; i < fmt.length; i++) {
      const ch = fmt[i];
      if (ch !== '%') { out += ch; continue; }
      i++;
      if (fmt[i] === '%') { out += '%'; continue; }
      let prec = -1;
      if (fmt[i] === '.') {
        i++;
        let digits = '';
        while (i < fmt.length && /[0-9]/.test(fmt[i])) digits += fmt[i++];
        prec = digits === '' ? 0 : parseInt(digits, 10);
      }
      while (fmt[i] === 'l' || fmt[i] === 'h') i++;
      const conv = fmt[i];
      const arg = ai < args.length ? args[ai++] : { n: 0 };
      if (conv === 'd' || conv === 'i') {
        out += String(truncateToInteger(arg.n || 0));
      } else if (conv === 'f') {
        out += (arg.n || 0).toFixed(prec === -1 ? 6 : prec);
      } else if (conv === 'c') {
        out += String.fromCharCode(truncateToInteger(arg.n || 0) & 0xff);
      } else if (conv === 's') {
        out += arg.s !== undefined ? (strings[arg.s] || '') : String(arg.n || 0);
      } else {
        out += '%' + (conv || '');
      }
    }
    return out;
  }

  // ---- boot: call main ----
  const mainPC = funcPC.get('main');
  if (mainPC === undefined) {
    runtimeError("No 'main' entry point in bytecode");
    return finish();
  }

  const mainFrameSize = frameSizeOf('main');
  frames.push({ func: 'main', retPC: -1, bp: sem.globalSlotCount });
  let memTop = sem.globalSlotCount + mainFrameSize;
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
      case 'PUSH_STR': pushStr(instr.operand); pc++; break;
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
      case 'JMP': pc = instr.operand; break;
      case 'JZ': { const v = pop(); pc = (v.n || 0) === 0 ? instr.operand : pc + 1; break; }
      case 'CALL': {
        const target = funcPC.get(instr.symbol);
        if (target === undefined) { runtimeError(`Call to undefined function '${instr.symbol}'`, instr.line); break; }
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
        for (let i = 0; i < nTargets; i++) {
          const raw = String(inputs[inputIdx++]).trim();
          let val = parseFloat(raw);
          if (Number.isNaN(val)) val = 0;
          if (fmt.includes('%d') && i === 0) val = truncateToInteger(val);
          const addr = truncateToInteger(addrs[i].n || 0);
          if (addr < 0 || addr >= memTop) { runtimeError(`Invalid scanf target address ${addr}`, instr.line); break; }
          mem[addr] = val;
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
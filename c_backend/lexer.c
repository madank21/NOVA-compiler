#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------------- */
/* String pool (arena)                                                        */
/* ------------------------------------------------------------------------- */

struct StrPool {
    char** items;
    int count;
    int capacity;
};

StrPool* strpool_new(void) {
    StrPool* p = (StrPool*)calloc(1, sizeof(StrPool));
    if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    p->capacity = 256;
    p->items = (char**)malloc(sizeof(char*) * (size_t)p->capacity);
    if (!p->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return p;
}

static void strpool_push(StrPool* pool, char* s) {
    if (pool->count >= pool->capacity) {
        pool->capacity *= 2;
        char** grown = (char**)realloc(pool->items, sizeof(char*) * (size_t)pool->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        pool->items = grown;
    }
    pool->items[pool->count++] = s;
}

char* strpool_ndup(StrPool* pool, const char* s, int n) {
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    strpool_push(pool, out);
    return out;
}

char* strpool_dup(StrPool* pool, const char* s) {
    return strpool_ndup(pool, s, (int)strlen(s));
}

void strpool_free(StrPool* pool) {
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) free(pool->items[i]);
    free(pool->items);
    free(pool);
}

/* ------------------------------------------------------------------------- */
/* Growable byte buffer for building lexemes                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char* data;
    int len;
    int capacity;
} Buf;

static void buf_init(Buf* b) {
    b->capacity = 64;
    b->len = 0;
    b->data = (char*)malloc((size_t)b->capacity);
    if (!b->data) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    b->data[0] = '\0';
}

static void buf_push(Buf* b, char c) {
    if (b->len + 1 >= b->capacity) {
        b->capacity *= 2;
        char* grown = (char*)realloc(b->data, (size_t)b->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        b->data = grown;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void buf_str(Buf* b, const char* s) {
    while (*s) buf_push(b, *s++);
}

static void buf_free(Buf* b) { free(b->data); }

/* ------------------------------------------------------------------------- */
/* Lexer                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char* src;
    int len;
    int pos;
    int line;
    int col;
} Lexer;

static char lx_peek(Lexer* L, int off) {
    int p = L->pos + off;
    if (p >= L->len) return '\0';
    return L->src[p];
}

static char lx_advance(Lexer* L) {
    if (L->pos >= L->len) return '\0';
    char c = L->src[L->pos++];
    if (c == '\n') { L->line++; L->col = 1; } else { L->col++; }
    return c;
}

static int is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit_c(char c) { return c >= '0' && c <= '9'; }
static int is_hex_c(char c) { return is_digit_c(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static const struct { const char* word; TokenType type; } KEYWORDS[] = {
    {"auto", TOKEN_AUTO}, {"break", TOKEN_BREAK}, {"case", TOKEN_CASE}, {"char", TOKEN_CHAR},
    {"const", TOKEN_CONST}, {"continue", TOKEN_CONTINUE}, {"default", TOKEN_DEFAULT}, {"do", TOKEN_DO},
    {"double", TOKEN_DOUBLE}, {"else", TOKEN_ELSE}, {"enum", TOKEN_ENUM}, {"extern", TOKEN_EXTERN},
    {"float", TOKEN_FLOAT}, {"for", TOKEN_FOR}, {"goto", TOKEN_GOTO}, {"if", TOKEN_IF},
    {"int", TOKEN_INT}, {"long", TOKEN_LONG}, {"register", TOKEN_REGISTER}, {"return", TOKEN_RETURN},
    {"short", TOKEN_SHORT}, {"signed", TOKEN_SIGNED}, {"sizeof", TOKEN_SIZEOF}, {"static", TOKEN_STATIC},
    {"struct", TOKEN_STRUCT}, {"switch", TOKEN_SWITCH}, {"typedef", TOKEN_TYPEDEF}, {"union", TOKEN_UNION},
    {"unsigned", TOKEN_UNSIGNED}, {"void", TOKEN_VOID}, {"volatile", TOKEN_VOLATILE}, {"while", TOKEN_WHILE},
};

static TokenType keyword_type(const char* word) {
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strcmp(KEYWORDS[i].word, word) == 0) return KEYWORDS[i].type;
    }
    return TOKEN_IDENTIFIER;
}

static void tl_push(TokenList* list, Token tok) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        Token* grown = (Token*)realloc(list->items, sizeof(Token) * (size_t)list->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        list->items = grown;
    }
    list->items[list->count++] = tok;
}

static Token make_token(TokenType type, char* lexeme, int line, int column) {
    Token t;
    memset(&t, 0, sizeof(Token));
    t.type = type;
    t.lexeme = lexeme;
    t.line = line;
    t.column = column;
    return t;
}

TokenList* tokenize(const char* source, DiagList* diags, StrPool* pool) {
    Lexer L;
    L.src = source;
    L.len = (int)strlen(source);
    L.pos = 0;
    L.line = 1;
    L.col = 1;

    TokenList* list = (TokenList*)calloc(1, sizeof(TokenList));
    if (!list) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    list->capacity = 128;
    list->items = (Token*)malloc(sizeof(Token) * (size_t)list->capacity);
    if (!list->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }

    while (L.pos < L.len) {
        char c = lx_peek(&L, 0);

        if (c == '\n' || c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            lx_advance(&L);
            continue;
        }

        /* Comments */
        if (c == '/' && lx_peek(&L, 1) == '/') {
            while (L.pos < L.len && lx_peek(&L, 0) != '\n') lx_advance(&L);
            continue;
        }
        if (c == '/' && lx_peek(&L, 1) == '*') {
            lx_advance(&L); lx_advance(&L);
            int closed = 0;
            while (L.pos < L.len) {
                if (lx_peek(&L, 0) == '*' && lx_peek(&L, 1) == '/') {
                    lx_advance(&L); lx_advance(&L); closed = 1; break;
                }
                lx_advance(&L);
            }
            if (!closed) diag_add(diags, "error", L.line, L.col, "Unterminated block comment");
            continue;
        }

        int start_line = L.line, start_col = L.col;

        /* Preprocessor */
        if (c == '#') {
            lx_advance(&L);
            Buf word; buf_init(&word);
            buf_push(&word, '#');
            while (L.pos < L.len && ((lx_peek(&L, 0) >= 'a' && lx_peek(&L, 0) <= 'z') ||
                                     (lx_peek(&L, 0) >= 'A' && lx_peek(&L, 0) <= 'Z'))) {
                buf_push(&word, lx_advance(&L));
            }
            TokenType tt = TOKEN_HASH;
            if (strcmp(word.data, "#include") == 0) tt = TOKEN_INCLUDE;
            else if (strcmp(word.data, "#define") == 0) tt = TOKEN_DEFINE;
            tl_push(list, make_token(tt, strpool_dup(pool, word.data), start_line, start_col));
            buf_free(&word);
            continue;
        }

        /* Identifier / keyword */
        if (is_ident_start(c)) {
            Buf lex; buf_init(&lex);
            while (L.pos < L.len && is_ident_char(lx_peek(&L, 0))) buf_push(&lex, lx_advance(&L));
            tl_push(list, make_token(keyword_type(lex.data), strpool_dup(pool, lex.data), start_line, start_col));
            buf_free(&lex);
            continue;
        }

        /* Number */
        if (is_digit_c(c) || (c == '.' && is_digit_c(lx_peek(&L, 1)))) {
            Buf lex; buf_init(&lex);
            int is_float = 0;
            if (c == '0' && (lx_peek(&L, 1) == 'x' || lx_peek(&L, 1) == 'X')) {
                buf_push(&lex, lx_advance(&L));
                buf_push(&lex, lx_advance(&L));
                while (L.pos < L.len && is_hex_c(lx_peek(&L, 0))) buf_push(&lex, lx_advance(&L));
                tl_push(list, make_token(TOKEN_INTEGER_LITERAL, strpool_dup(pool, lex.data), start_line, start_col));
                buf_free(&lex);
                continue;
            }
            while (L.pos < L.len && is_digit_c(lx_peek(&L, 0))) buf_push(&lex, lx_advance(&L));
            if (lx_peek(&L, 0) == '.' && is_digit_c(lx_peek(&L, 1))) {
                is_float = 1;
                buf_push(&lex, lx_advance(&L));
                while (L.pos < L.len && is_digit_c(lx_peek(&L, 0))) buf_push(&lex, lx_advance(&L));
            } else if (lx_peek(&L, 0) == '.' && lex.len > 0 &&
                       lx_peek(&L, 1) != 'e' && lx_peek(&L, 1) != 'E' && lx_peek(&L, 1) != '.') {
                char nx = lx_peek(&L, 1);
                if (!is_ident_start(nx)) { is_float = 1; buf_push(&lex, lx_advance(&L)); }
            }
            if (lx_peek(&L, 0) == 'e' || lx_peek(&L, 0) == 'E') {
                int save_pos = L.pos, save_line = L.line, save_col = L.col;
                Buf exp; buf_init(&exp);
                buf_push(&exp, lx_advance(&L));
                if (lx_peek(&L, 0) == '+' || lx_peek(&L, 0) == '-') buf_push(&exp, lx_advance(&L));
                if (is_digit_c(lx_peek(&L, 0))) {
                    is_float = 1;
                    buf_str(&lex, exp.data);
                    while (L.pos < L.len && is_digit_c(lx_peek(&L, 0))) buf_push(&lex, lx_advance(&L));
                } else {
                    L.pos = save_pos; L.line = save_line; L.col = save_col;
                }
                buf_free(&exp);
            }
            if (lx_peek(&L, 0) == 'f' || lx_peek(&L, 0) == 'F' ||
                lx_peek(&L, 0) == 'l' || lx_peek(&L, 0) == 'L') lx_advance(&L);
            tl_push(list, make_token(is_float ? TOKEN_FLOAT_LITERAL : TOKEN_INTEGER_LITERAL,
                                     strpool_dup(pool, lex.data), start_line, start_col));
            buf_free(&lex);
            continue;
        }

        /* Character literal */
        if (c == '\'') {
            lx_advance(&L);
            int value = 0;
            Buf body; buf_init(&body);
            int closed = 0;
            while (L.pos < L.len && lx_peek(&L, 0) != '\'' && lx_peek(&L, 0) != '\n') {
                char ch = lx_advance(&L);
                if (ch == '\\' && L.pos < L.len) {
                    char esc = lx_advance(&L);
                    if (esc == 'n') value = 10;
                    else if (esc == 't') value = 9;
                    else if (esc == '0') value = 0;
                    else if (esc == 'r') value = 13;
                    else if (esc == '\\') value = 92;
                    else if (esc == '\'') value = 39;
                    else value = (unsigned char)esc;
                    buf_push(&body, '\\');
                    buf_push(&body, esc);
                } else {
                    value = (unsigned char)ch;
                    buf_push(&body, ch);
                }
            }
            if (L.pos < L.len && lx_peek(&L, 0) == '\'') { lx_advance(&L); closed = 1; }
            if (!closed) diag_add(diags, "error", start_line, start_col, "Unterminated character literal");
            Buf lex; buf_init(&lex);
            buf_push(&lex, '\'');
            buf_str(&lex, body.data);
            buf_push(&lex, '\'');
            Token tok = make_token(TOKEN_CHAR_LITERAL, strpool_dup(pool, lex.data), start_line, start_col);
            tok.char_value = value;
            tl_push(list, tok);
            buf_free(&body);
            buf_free(&lex);
            continue;
        }

        /* String literal */
        if (c == '"') {
            lx_advance(&L);
            Buf value; buf_init(&value);
            Buf lex; buf_init(&lex);
            buf_push(&lex, '"');
            int closed = 0;
            while (L.pos < L.len && lx_peek(&L, 0) != '"' && lx_peek(&L, 0) != '\n') {
                char ch = lx_advance(&L);
                if (ch == '\\' && L.pos < L.len) {
                    char esc = lx_advance(&L);
                    if (esc == 'n') buf_push(&value, '\n');
                    else if (esc == 't') buf_push(&value, '\t');
                    else if (esc == '0') buf_push(&value, '\0');
                    else if (esc == 'r') buf_push(&value, '\r');
                    else buf_push(&value, esc);
                    buf_push(&lex, '\\');
                    buf_push(&lex, esc);
                } else {
                    buf_push(&value, ch);
                    buf_push(&lex, ch);
                }
            }
            if (L.pos < L.len && lx_peek(&L, 0) == '"') { lx_advance(&L); buf_push(&lex, '"'); closed = 1; }
            if (!closed) diag_add(diags, "error", start_line, start_col, "Unterminated string literal");
            Token tok = make_token(TOKEN_STRING_LITERAL, strpool_dup(pool, lex.data), start_line, start_col);
            tok.string_value = strpool_dup(pool, value.data);
            tl_push(list, tok);
            buf_free(&value);
            buf_free(&lex);
            continue;
        }

        /* Two-character operators */
        {
            char two[3] = { c, lx_peek(&L, 1), '\0' };
            TokenType tt = TOKEN_ERROR;
            int matched = 1;
            if (strcmp(two, "++") == 0) tt = TOKEN_PLUS_PLUS;
            else if (strcmp(two, "--") == 0) tt = TOKEN_MINUS_MINUS;
            else if (strcmp(two, "==") == 0) tt = TOKEN_EQ;
            else if (strcmp(two, "!=") == 0) tt = TOKEN_NEQ;
            else if (strcmp(two, "<=") == 0) tt = TOKEN_LEQ;
            else if (strcmp(two, ">=") == 0) tt = TOKEN_GEQ;
            else if (strcmp(two, "&&") == 0) tt = TOKEN_AND;
            else if (strcmp(two, "||") == 0) tt = TOKEN_OR;
            else if (strcmp(two, "+=") == 0) tt = TOKEN_PLUS_ASSIGN;
            else if (strcmp(two, "-=") == 0) tt = TOKEN_MINUS_ASSIGN;
            else if (strcmp(two, "*=") == 0) tt = TOKEN_STAR_ASSIGN;
            else if (strcmp(two, "/=") == 0) tt = TOKEN_SLASH_ASSIGN;
            else if (strcmp(two, "%=") == 0) tt = TOKEN_PERCENT_ASSIGN;
            else if (strcmp(two, "->") == 0) tt = TOKEN_ARROW;
            else if (strcmp(two, "<<") == 0) tt = TOKEN_LSHIFT;
            else if (strcmp(two, ">>") == 0) tt = TOKEN_RSHIFT;
            else matched = 0;
            if (matched) {
                lx_advance(&L); lx_advance(&L);
                tl_push(list, make_token(tt, strpool_dup(pool, two), start_line, start_col));
                continue;
            }
        }

        /* Single-character operators / separators */
        {
            TokenType tt = TOKEN_ERROR;
            int matched = 1;
            switch (c) {
                case '+': tt = TOKEN_PLUS; break;
                case '-': tt = TOKEN_MINUS; break;
                case '*': tt = TOKEN_STAR; break;
                case '/': tt = TOKEN_SLASH; break;
                case '%': tt = TOKEN_PERCENT; break;
                case '=': tt = TOKEN_ASSIGN; break;
                case '!': tt = TOKEN_NOT; break;
                case '<': tt = TOKEN_LT; break;
                case '>': tt = TOKEN_GT; break;
                case '&': tt = TOKEN_AMPERSAND; break;
                case '|': tt = TOKEN_BIT_OR; break;
                case '^': tt = TOKEN_BIT_XOR; break;
                case '~': tt = TOKEN_BIT_NOT; break;
                case '?': tt = TOKEN_QUESTION; break;
                case ':': tt = TOKEN_COLON; break;
                case '(': tt = TOKEN_LPAREN; break;
                case ')': tt = TOKEN_RPAREN; break;
                case '{': tt = TOKEN_LBRACE; break;
                case '}': tt = TOKEN_RBRACE; break;
                case '[': tt = TOKEN_LBRACKET; break;
                case ']': tt = TOKEN_RBRACKET; break;
                case ';': tt = TOKEN_SEMICOLON; break;
                case ',': tt = TOKEN_COMMA; break;
                case '.': tt = TOKEN_DOT; break;
                default: matched = 0; break;
            }
            if (matched) {
                lx_advance(&L);
                char one[2] = { c, '\0' };
                tl_push(list, make_token(tt, strpool_dup(pool, one), start_line, start_col));
                continue;
            }
        }

        lx_advance(&L);
        diag_add(diags, "error", start_line, start_col, "Unexpected character '%c'", c);
        char one[2] = { c, '\0' };
        tl_push(list, make_token(TOKEN_ERROR, strpool_dup(pool, one), start_line, start_col));
    }

    tl_push(list, make_token(TOKEN_EOF, strpool_dup(pool, "EOF"), L.line, L.col));
    return list;
}

void token_list_free(TokenList* list) {
    if (!list) return;
    free(list->items);
    free(list);
}

const char* token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_AUTO: return "TOKEN_AUTO";
        case TOKEN_BREAK: return "TOKEN_BREAK";
        case TOKEN_CASE: return "TOKEN_CASE";
        case TOKEN_CHAR: return "TOKEN_CHAR";
        case TOKEN_CONST: return "TOKEN_CONST";
        case TOKEN_CONTINUE: return "TOKEN_CONTINUE";
        case TOKEN_DEFAULT: return "TOKEN_DEFAULT";
        case TOKEN_DO: return "TOKEN_DO";
        case TOKEN_DOUBLE: return "TOKEN_DOUBLE";
        case TOKEN_ELSE: return "TOKEN_ELSE";
        case TOKEN_ENUM: return "TOKEN_ENUM";
        case TOKEN_EXTERN: return "TOKEN_EXTERN";
        case TOKEN_FLOAT: return "TOKEN_FLOAT";
        case TOKEN_FOR: return "TOKEN_FOR";
        case TOKEN_GOTO: return "TOKEN_GOTO";
        case TOKEN_IF: return "TOKEN_IF";
        case TOKEN_INT: return "TOKEN_INT";
        case TOKEN_LONG: return "TOKEN_LONG";
        case TOKEN_REGISTER: return "TOKEN_REGISTER";
        case TOKEN_RETURN: return "TOKEN_RETURN";
        case TOKEN_SHORT: return "TOKEN_SHORT";
        case TOKEN_SIGNED: return "TOKEN_SIGNED";
        case TOKEN_SIZEOF: return "TOKEN_SIZEOF";
        case TOKEN_STATIC: return "TOKEN_STATIC";
        case TOKEN_STRUCT: return "TOKEN_STRUCT";
        case TOKEN_SWITCH: return "TOKEN_SWITCH";
        case TOKEN_TYPEDEF: return "TOKEN_TYPEDEF";
        case TOKEN_UNION: return "TOKEN_UNION";
        case TOKEN_UNSIGNED: return "TOKEN_UNSIGNED";
        case TOKEN_VOID: return "TOKEN_VOID";
        case TOKEN_VOLATILE: return "TOKEN_VOLATILE";
        case TOKEN_WHILE: return "TOKEN_WHILE";
        case TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TOKEN_INTEGER_LITERAL: return "TOKEN_INTEGER_LITERAL";
        case TOKEN_FLOAT_LITERAL: return "TOKEN_FLOAT_LITERAL";
        case TOKEN_CHAR_LITERAL: return "TOKEN_CHAR_LITERAL";
        case TOKEN_STRING_LITERAL: return "TOKEN_STRING_LITERAL";
        case TOKEN_PLUS: return "TOKEN_PLUS";
        case TOKEN_MINUS: return "TOKEN_MINUS";
        case TOKEN_STAR: return "TOKEN_STAR";
        case TOKEN_SLASH: return "TOKEN_SLASH";
        case TOKEN_PERCENT: return "TOKEN_PERCENT";
        case TOKEN_PLUS_PLUS: return "TOKEN_PLUS_PLUS";
        case TOKEN_MINUS_MINUS: return "TOKEN_MINUS_MINUS";
        case TOKEN_ASSIGN: return "TOKEN_ASSIGN";
        case TOKEN_PLUS_ASSIGN: return "TOKEN_PLUS_ASSIGN";
        case TOKEN_MINUS_ASSIGN: return "TOKEN_MINUS_ASSIGN";
        case TOKEN_STAR_ASSIGN: return "TOKEN_STAR_ASSIGN";
        case TOKEN_SLASH_ASSIGN: return "TOKEN_SLASH_ASSIGN";
        case TOKEN_PERCENT_ASSIGN: return "TOKEN_PERCENT_ASSIGN";
        case TOKEN_EQ: return "TOKEN_EQ";
        case TOKEN_NEQ: return "TOKEN_NEQ";
        case TOKEN_LT: return "TOKEN_LT";
        case TOKEN_GT: return "TOKEN_GT";
        case TOKEN_LEQ: return "TOKEN_LEQ";
        case TOKEN_GEQ: return "TOKEN_GEQ";
        case TOKEN_AND: return "TOKEN_AND";
        case TOKEN_OR: return "TOKEN_OR";
        case TOKEN_NOT: return "TOKEN_NOT";
        case TOKEN_BIT_AND: return "TOKEN_BIT_AND";
        case TOKEN_BIT_OR: return "TOKEN_BIT_OR";
        case TOKEN_BIT_XOR: return "TOKEN_BIT_XOR";
        case TOKEN_BIT_NOT: return "TOKEN_BIT_NOT";
        case TOKEN_LSHIFT: return "TOKEN_LSHIFT";
        case TOKEN_RSHIFT: return "TOKEN_RSHIFT";
        case TOKEN_QUESTION: return "TOKEN_QUESTION";
        case TOKEN_COLON: return "TOKEN_COLON";
        case TOKEN_LPAREN: return "TOKEN_LPAREN";
        case TOKEN_RPAREN: return "TOKEN_RPAREN";
        case TOKEN_LBRACE: return "TOKEN_LBRACE";
        case TOKEN_RBRACE: return "TOKEN_RBRACE";
        case TOKEN_LBRACKET: return "TOKEN_LBRACKET";
        case TOKEN_RBRACKET: return "TOKEN_RBRACKET";
        case TOKEN_SEMICOLON: return "TOKEN_SEMICOLON";
        case TOKEN_COMMA: return "TOKEN_COMMA";
        case TOKEN_DOT: return "TOKEN_DOT";
        case TOKEN_ARROW: return "TOKEN_ARROW";
        case TOKEN_HASH: return "TOKEN_HASH";
        case TOKEN_INCLUDE: return "TOKEN_INCLUDE";
        case TOKEN_DEFINE: return "TOKEN_DEFINE";
        case TOKEN_AMPERSAND: return "TOKEN_AMPERSAND";
        case TOKEN_EOF: return "TOKEN_EOF";
        case TOKEN_ERROR: return "TOKEN_ERROR";
        default: return "TOKEN_ERROR";
    }
}
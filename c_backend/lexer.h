#ifndef NOVA_LEXER_H
#define NOVA_LEXER_H

#include "diag.h"

/* Token type names MUST stay identical to the browser engine's strings
 * (src/engine/compilerEngine.js) — they are part of the JSON contract. */
typedef enum {
    TOKEN_AUTO, TOKEN_BREAK, TOKEN_CASE, TOKEN_CHAR,
    TOKEN_CONST, TOKEN_CONTINUE, TOKEN_DEFAULT, TOKEN_DO,
    TOKEN_DOUBLE, TOKEN_ELSE, TOKEN_ENUM, TOKEN_EXTERN,
    TOKEN_FLOAT, TOKEN_FOR, TOKEN_GOTO, TOKEN_IF,
    TOKEN_INT, TOKEN_LONG, TOKEN_REGISTER, TOKEN_RETURN,
    TOKEN_SHORT, TOKEN_SIGNED, TOKEN_SIZEOF, TOKEN_STATIC,
    TOKEN_STRUCT, TOKEN_SWITCH, TOKEN_TYPEDEF, TOKEN_UNION,
    TOKEN_UNSIGNED, TOKEN_VOID, TOKEN_VOLATILE, TOKEN_WHILE,

    TOKEN_IDENTIFIER,
    TOKEN_INTEGER_LITERAL,
    TOKEN_FLOAT_LITERAL,
    TOKEN_CHAR_LITERAL,
    TOKEN_STRING_LITERAL,

    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,

    TOKEN_ASSIGN, TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN,
    TOKEN_STAR_ASSIGN, TOKEN_SLASH_ASSIGN, TOKEN_PERCENT_ASSIGN,

    TOKEN_EQ, TOKEN_NEQ, TOKEN_LT, TOKEN_GT, TOKEN_LEQ, TOKEN_GEQ,
    TOKEN_AND, TOKEN_OR, TOKEN_NOT,
    TOKEN_BIT_AND, TOKEN_BIT_OR, TOKEN_BIT_XOR, TOKEN_BIT_NOT,
    TOKEN_LSHIFT, TOKEN_RSHIFT,
    TOKEN_QUESTION, TOKEN_COLON,
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_LBRACKET, TOKEN_RBRACKET, TOKEN_SEMICOLON, TOKEN_COMMA,
    TOKEN_DOT, TOKEN_ARROW,
    TOKEN_HASH, TOKEN_INCLUDE, TOKEN_DEFINE,
    TOKEN_AMPERSAND,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char* lexeme;        /* owned by the compile arena */
    int line;
    int column;
    int char_value;      /* TOKEN_CHAR_LITERAL: character code */
    char* string_value;  /* TOKEN_STRING_LITERAL: decoded value */
} Token;

typedef struct {
    Token* items;
    int count;
    int capacity;
} TokenList;

/* Arena for strings that must live for the whole compilation. */
typedef struct StrPool StrPool;
StrPool* strpool_new(void);
char* strpool_dup(StrPool* pool, const char* s);
char* strpool_ndup(StrPool* pool, const char* s, int n);
void strpool_free(StrPool* pool);

TokenList* tokenize(const char* source, DiagList* diags, StrPool* pool);
void token_list_free(TokenList* list);
const char* token_type_name(TokenType type);

#endif

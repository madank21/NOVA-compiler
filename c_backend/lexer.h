#ifndef NOVA_LEXER_H
#define NOVA_LEXER_H

#include "diag.h"
#include <stddef.h>

/* Token type names are part of the JSON contract and must match the JS engine. */
typedef enum {
    TOK_AUTO, TOK_BREAK, TOK_CASE, TOK_CHAR, TOK_CONST, TOK_CONTINUE, TOK_DEFAULT,
    TOK_DO, TOK_DOUBLE, TOK_ELSE, TOK_ENUM, TOK_EXTERN, TOK_FLOAT, TOK_FOR, TOK_GOTO,
    TOK_IF, TOK_INT, TOK_LONG, TOK_REGISTER, TOK_RETURN, TOK_SHORT, TOK_SIGNED,
    TOK_SIZEOF, TOK_STATIC, TOK_STRUCT, TOK_SWITCH, TOK_TYPEDEF, TOK_UNION,
    TOK_UNSIGNED, TOK_VOID, TOK_VOLATILE, TOK_WHILE,
    TOK_IDENTIFIER, TOK_INTEGER_LITERAL, TOK_FLOAT_LITERAL, TOK_CHAR_LITERAL,
    TOK_STRING_LITERAL,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_PLUS_PLUS, TOK_MINUS_MINUS,
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN,
    TOK_PERCENT_ASSIGN, TOK_AND_ASSIGN, TOK_OR_ASSIGN, TOK_XOR_ASSIGN,
    TOK_LSHIFT_ASSIGN, TOK_RSHIFT_ASSIGN,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LEQ, TOK_GEQ,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_BIT_AND, TOK_BIT_OR, TOK_BIT_XOR, TOK_BIT_NOT, TOK_LSHIFT, TOK_RSHIFT,
    TOK_QUESTION, TOK_COLON,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMICOLON, TOK_COMMA, TOK_DOT, TOK_ARROW,
    TOK_INCLUDE, TOK_DEFINE, TOK_HASH, TOK_ELLIPSIS,
    TOK_EOF, TOK_ERROR
} NovaTokenType;

typedef struct {
    NovaTokenType type;
    char lexeme[256];
    int line;
    int column;
    long long int_value;      /* integer literals */
    double float_value;       /* float literals */
    int char_value;           /* char literals */
    char *string_value;       /* string literals (heap) */
} NovaToken;

typedef struct {
    NovaToken *items;
    int count;
    int capacity;
} NovaTokenList;

NovaTokenList *nova_tokenize(const char *source, DiagList *diags);
void nova_token_list_free(NovaTokenList *list);
const char *nova_token_type_name(NovaTokenType type);

#endif

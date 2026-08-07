#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef enum {
    // Keywords
    TOKEN_AUTO, TOKEN_BREAK, TOKEN_CASE, TOKEN_CHAR,
    TOKEN_CONST, TOKEN_CONTINUE, TOKEN_DEFAULT, TOKEN_DO,
    TOKEN_DOUBLE, TOKEN_ELSE, TOKEN_ENUM, TOKEN_EXTERN,
    TOKEN_FLOAT, TOKEN_FOR, TOKEN_GOTO, TOKEN_IF,
    TOKEN_INT, TOKEN_LONG, TOKEN_REGISTER, TOKEN_RETURN,
    TOKEN_SHORT, TOKEN_SIGNED, TOKEN_SIZEOF, TOKEN_STATIC,
    TOKEN_STRUCT, TOKEN_SWITCH, TOKEN_TYPEDEF, TOKEN_UNION,
    TOKEN_UNSIGNED, TOKEN_VOID, TOKEN_VOLATILE, TOKEN_WHILE,

    // Identifiers and Literals
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER_LITERAL,
    TOKEN_FLOAT_LITERAL,
    TOKEN_CHAR_LITERAL,
    TOKEN_STRING_LITERAL,

    // Arithmetic Operators
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,

    // Assignment Operators
    TOKEN_ASSIGN, TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN,
    TOKEN_STAR_ASSIGN, TOKEN_SLASH_ASSIGN, TOKEN_PERCENT_ASSIGN,
    TOKEN_AND_ASSIGN, TOKEN_OR_ASSIGN, TOKEN_XOR_ASSIGN,
    TOKEN_LSHIFT_ASSIGN, TOKEN_RSHIFT_ASSIGN,

    // Relational Operators
    TOKEN_EQ, TOKEN_NEQ, TOKEN_LT, TOKEN_GT, TOKEN_LEQ, TOKEN_GEQ,

    // Logical Operators
    TOKEN_AND, TOKEN_OR, TOKEN_NOT,

    // Bitwise Operators
    TOKEN_BIT_AND, TOKEN_BIT_OR, TOKEN_BIT_XOR, TOKEN_BIT_NOT,
    TOKEN_LSHIFT, TOKEN_RSHIFT,

    // Ternary & Separators
    TOKEN_QUESTION, TOKEN_COLON,
    TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_DOT, TOKEN_ARROW,

    // Preprocessor
    TOKEN_HASH, TOKEN_INCLUDE, TOKEN_DEFINE,

    // Special
    TOKEN_AMPERSAND,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char lexeme[256];
    int line;
    int column;
    union {
        long long int_value;
        double float_value;
        char char_value;
        char string_value[1024];
    } value;
} Token;

typedef struct {
    Token* tokens;
    int count;
    int capacity;
} TokenList;

typedef struct {
    const char* source;
    int source_len;
    int pos;
    int line;
    int column;
} Lexer;

Lexer* lexer_init(const char* source);
void lexer_free(Lexer* lexer);
TokenList* lexer_tokenize(Lexer* lexer);
void token_list_free(TokenList* list);
const char* token_type_to_string(TokenType type);

#endif // LEXER_H

#include "lexer.h"

Lexer* lexer_init(const char* source) {
    Lexer* lexer = (Lexer*)malloc(sizeof(Lexer));
    lexer->source = source;
    lexer->source_len = (int)strlen(source);
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    return lexer;
}

void lexer_free(Lexer* lexer) {
    if (lexer) free(lexer);
}

static char peek_char(Lexer* lexer) {
    if (lexer->pos >= lexer->source_len) return '\0';
    return lexer->source[lexer->pos];
}

static char peek_next_char(Lexer* lexer) {
    if (lexer->pos + 1 >= lexer->source_len) return '\0';
    return lexer->source[lexer->pos + 1];
}

static char advance_char(Lexer* lexer) {
    if (lexer->pos >= lexer->source_len) return '\0';
    char c = lexer->source[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static void skip_whitespace_and_comments(Lexer* lexer) {
    while (lexer->pos < lexer->source_len) {
        char c = peek_char(lexer);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_char(lexer);
        } else if (c == '/' && peek_next_char(lexer) == '/') {
            advance_char(lexer);
            advance_char(lexer);
            while (lexer->pos < lexer->source_len && peek_char(lexer) != '\n') {
                advance_char(lexer);
            }
        } else if (c == '/' && peek_next_char(lexer) == '*') {
            advance_char(lexer);
            advance_char(lexer);
            while (lexer->pos < lexer->source_len) {
                if (peek_char(lexer) == '*' && peek_next_char(lexer) == '/') {
                    advance_char(lexer);
                    advance_char(lexer);
                    break;
                }
                advance_char(lexer);
            }
        } else {
            break;
        }
    }
}

static TokenType check_keyword(const char* str) {
    if (strcmp(str, "auto") == 0) return TOKEN_AUTO;
    if (strcmp(str, "break") == 0) return TOKEN_BREAK;
    if (strcmp(str, "case") == 0) return TOKEN_CASE;
    if (strcmp(str, "char") == 0) return TOKEN_CHAR;
    if (strcmp(str, "const") == 0) return TOKEN_CONST;
    if (strcmp(str, "continue") == 0) return TOKEN_CONTINUE;
    if (strcmp(str, "default") == 0) return TOKEN_DEFAULT;
    if (strcmp(str, "do") == 0) return TOKEN_DO;
    if (strcmp(str, "double") == 0) return TOKEN_DOUBLE;
    if (strcmp(str, "else") == 0) return TOKEN_ELSE;
    if (strcmp(str, "enum") == 0) return TOKEN_ENUM;
    if (strcmp(str, "extern") == 0) return TOKEN_EXTERN;
    if (strcmp(str, "float") == 0) return TOKEN_FLOAT;
    if (strcmp(str, "for") == 0) return TOKEN_FOR;
    if (strcmp(str, "goto") == 0) return TOKEN_GOTO;
    if (strcmp(str, "if") == 0) return TOKEN_IF;
    if (strcmp(str, "int") == 0) return TOKEN_INT;
    if (strcmp(str, "long") == 0) return TOKEN_LONG;
    if (strcmp(str, "register") == 0) return TOKEN_REGISTER;
    if (strcmp(str, "return") == 0) return TOKEN_RETURN;
    if (strcmp(str, "short") == 0) return TOKEN_SHORT;
    if (strcmp(str, "signed") == 0) return TOKEN_SIGNED;
    if (strcmp(str, "sizeof") == 0) return TOKEN_SIZEOF;
    if (strcmp(str, "static") == 0) return TOKEN_STATIC;
    if (strcmp(str, "struct") == 0) return TOKEN_STRUCT;
    if (strcmp(str, "switch") == 0) return TOKEN_SWITCH;
    if (strcmp(str, "typedef") == 0) return TOKEN_TYPEDEF;
    if (strcmp(str, "union") == 0) return TOKEN_UNION;
    if (strcmp(str, "unsigned") == 0) return TOKEN_UNSIGNED;
    if (strcmp(str, "void") == 0) return TOKEN_VOID;
    if (strcmp(str, "volatile") == 0) return TOKEN_VOLATILE;
    if (strcmp(str, "while") == 0) return TOKEN_WHILE;
    return TOKEN_IDENTIFIER;
}

TokenList* lexer_tokenize(Lexer* lexer) {
    TokenList* list = (TokenList*)malloc(sizeof(TokenList));
    list->capacity = 64;
    list->count = 0;
    list->tokens = (Token*)malloc(sizeof(Token) * list->capacity);

    while (1) {
        skip_whitespace_and_comments(lexer);

        if (lexer->pos >= lexer->source_len) {
            Token tok;
            tok.type = TOKEN_EOF;
            strcpy(tok.lexeme, "EOF");
            tok.line = lexer->line;
            tok.column = lexer->column;
            if (list->count >= list->capacity) {
                list->capacity *= 2;
                list->tokens = (Token*)realloc(list->tokens, sizeof(Token) * list->capacity);
            }
            list->tokens[list->count++] = tok;
            break;
        }

        int start_line = lexer->line;
        int start_col = lexer->column;
        char c = peek_char(lexer);

        Token tok;
        tok.line = start_line;
        tok.column = start_col;
        tok.lexeme[0] = '\0';

        if (isalpha(c) || c == '_') {
            int len = 0;
            while (isalnum(peek_char(lexer)) || peek_char(lexer) == '_') {
                if (len < 255) tok.lexeme[len++] = advance_char(lexer);
                else advance_char(lexer);
            }
            tok.lexeme[len] = '\0';
            tok.type = check_keyword(tok.lexeme);
        } else if (isdigit(c)) {
            int len = 0;
            int is_float = 0;
            while (isdigit(peek_char(lexer)) || peek_char(lexer) == '.') {
                if (peek_char(lexer) == '.') is_float = 1;
                if (len < 255) tok.lexeme[len++] = advance_char(lexer);
                else advance_char(lexer);
            }
            tok.lexeme[len] = '\0';
            if (is_float) {
                tok.type = TOKEN_FLOAT_LITERAL;
                tok.value.float_value = atof(tok.lexeme);
            } else {
                tok.type = TOKEN_INTEGER_LITERAL;
                tok.value.int_value = atoll(tok.lexeme);
            }
        } else if (c == '"') {
            advance_char(lexer);
            int len = 0;
            while (peek_char(lexer) != '"' && peek_char(lexer) != '\0') {
                char char_val = advance_char(lexer);
                if (char_val == '\\' && peek_char(lexer) != '\0') {
                    char esc = advance_char(lexer);
                    if (esc == 'n') char_val = '\n';
                    else if (esc == 't') char_val = '\t';
                    else if (esc == '0') char_val = '\0';
                    else char_val = esc;
                }
                if (len < 1023) tok.value.string_value[len++] = char_val;
            }
            tok.value.string_value[len] = '\0';
            if (peek_char(lexer) == '"') advance_char(lexer);
            tok.type = TOKEN_STRING_LITERAL;
            snprintf(tok.lexeme, sizeof(tok.lexeme), "\"%s\"", tok.value.string_value);
        } else if (c == '#') {
            int len = 0;
            while (isalpha(peek_char(lexer)) || peek_char(lexer) == '#') {
                if (len < 255) tok.lexeme[len++] = advance_char(lexer);
                else advance_char(lexer);
            }
            tok.lexeme[len] = '\0';
            if (strcmp(tok.lexeme, "#include") == 0) tok.type = TOKEN_INCLUDE;
            else if (strcmp(tok.lexeme, "#define") == 0) tok.type = TOKEN_DEFINE;
            else tok.type = TOKEN_HASH;
        } else {
            char curr = advance_char(lexer);
            char next = peek_char(lexer);

            tok.lexeme[0] = curr;
            tok.lexeme[1] = '\0';

            switch (curr) {
                case '+':
                    if (next == '+') { advance_char(lexer); tok.type = TOKEN_PLUS_PLUS; strcpy(tok.lexeme, "++"); }
                    else if (next == '=') { advance_char(lexer); tok.type = TOKEN_PLUS_ASSIGN; strcpy(tok.lexeme, "+="); }
                    else tok.type = TOKEN_PLUS;
                    break;
                case '-':
                    if (next == '-') { advance_char(lexer); tok.type = TOKEN_MINUS_MINUS; strcpy(tok.lexeme, "--"); }
                    else if (next == '=') { advance_char(lexer); tok.type = TOKEN_MINUS_ASSIGN; strcpy(tok.lexeme, "-="); }
                    else if (next == '>') { advance_char(lexer); tok.type = TOKEN_ARROW; strcpy(tok.lexeme, "->"); }
                    else tok.type = TOKEN_MINUS;
                    break;
                case '*':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_STAR_ASSIGN; strcpy(tok.lexeme, "*="); }
                    else tok.type = TOKEN_STAR;
                    break;
                case '/':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_SLASH_ASSIGN; strcpy(tok.lexeme, "/="); }
                    else tok.type = TOKEN_SLASH;
                    break;
                case '%':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_PERCENT_ASSIGN; strcpy(tok.lexeme, "%="); }
                    else tok.type = TOKEN_PERCENT;
                    break;
                case '=':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_EQ; strcpy(tok.lexeme, "=="); }
                    else tok.type = TOKEN_ASSIGN;
                    break;
                case '!':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_NEQ; strcpy(tok.lexeme, "!="); }
                    else tok.type = TOKEN_NOT;
                    break;
                case '<':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_LEQ; strcpy(tok.lexeme, "<="); }
                    else if (next == '<') { advance_char(lexer); tok.type = TOKEN_LSHIFT; strcpy(tok.lexeme, "<<"); }
                    else tok.type = TOKEN_LT;
                    break;
                case '>':
                    if (next == '=') { advance_char(lexer); tok.type = TOKEN_GEQ; strcpy(tok.lexeme, ">="); }
                    else if (next == '>') { advance_char(lexer); tok.type = TOKEN_RSHIFT; strcpy(tok.lexeme, ">>"); }
                    else tok.type = TOKEN_GT;
                    break;
                case '&':
                    if (next == '&') { advance_char(lexer); tok.type = TOKEN_AND; strcpy(tok.lexeme, "&&"); }
                    else tok.type = TOKEN_AMPERSAND;
                    break;
                case '|':
                    if (next == '|') { advance_char(lexer); tok.type = TOKEN_OR; strcpy(tok.lexeme, "||"); }
                    else tok.type = TOKEN_BIT_OR;
                    break;
                case '(': tok.type = TOKEN_LPAREN; break;
                case ')': tok.type = TOKEN_RPAREN; break;
                case '{': tok.type = TOKEN_LBRACE; break;
                case '}': tok.type = TOKEN_RBRACE; break;
                case '[': tok.type = TOKEN_LBRACKET; break;
                case ']': tok.type = TOKEN_RBRACKET; break;
                case ';': tok.type = TOKEN_SEMICOLON; break;
                case ',': tok.type = TOKEN_COMMA; break;
                case '.': tok.type = TOKEN_DOT; break;
                default: tok.type = TOKEN_ERROR; break;
            }
        }

        if (list->count >= list->capacity) {
            list->capacity *= 2;
            list->tokens = (Token*)realloc(list->tokens, sizeof(Token) * list->capacity);
        }
        list->tokens[list->count++] = tok;
    }

    return list;
}

void token_list_free(TokenList* list) {
    if (list) {
        if (list->tokens) free(list->tokens);
        free(list);
    }
}

const char* token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_INT: return "TOKEN_INT";
        case TOKEN_FLOAT: return "TOKEN_FLOAT";
        case TOKEN_CHAR: return "TOKEN_CHAR";
        case TOKEN_VOID: return "TOKEN_VOID";
        case TOKEN_IF: return "TOKEN_IF";
        case TOKEN_ELSE: return "TOKEN_ELSE";
        case TOKEN_WHILE: return "TOKEN_WHILE";
        case TOKEN_FOR: return "TOKEN_FOR";
        case TOKEN_RETURN: return "TOKEN_RETURN";
        case TOKEN_STRUCT: return "TOKEN_STRUCT";
        case TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TOKEN_INTEGER_LITERAL: return "TOKEN_INTEGER_LITERAL";
        case TOKEN_FLOAT_LITERAL: return "TOKEN_FLOAT_LITERAL";
        case TOKEN_STRING_LITERAL: return "TOKEN_STRING_LITERAL";
        case TOKEN_PLUS: return "TOKEN_PLUS";
        case TOKEN_MINUS: return "TOKEN_MINUS";
        case TOKEN_STAR: return "TOKEN_STAR";
        case TOKEN_SLASH: return "TOKEN_SLASH";
        case TOKEN_ASSIGN: return "TOKEN_ASSIGN";
        case TOKEN_EQ: return "TOKEN_EQ";
        case TOKEN_SEMICOLON: return "TOKEN_SEMICOLON";
        case TOKEN_EOF: return "TOKEN_EOF";
        default: return "TOKEN_OPERATOR_OR_KEYWORD";
    }
}

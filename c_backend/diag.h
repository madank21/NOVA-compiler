#ifndef NOVA_DIAG_H
#define NOVA_DIAG_H

/* Diagnostic sink shared by every compiler phase. Levels: error, warning,
 * runtime. Mirrors src/engine/compilerEngine.js DiagList exactly. */

typedef struct {
    const char* level;
    int line;
    int column;
    char msg[256];
} Diag;

/* Mirrors DIAGNOSTIC_LIMIT in src/engine/compilerEngine.js. */
#define NOVA_DIAGNOSTIC_LIMIT 100

typedef struct {
    Diag* items;
    int count;
    int capacity;
    int limit_note_added;
} DiagList;

DiagList* diag_list_new(void);
void diag_add(DiagList* list, const char* level, int line, int column, const char* fmt, ...);
int diag_has_errors(const DiagList* list);
void diag_list_free(DiagList* list);

#endif

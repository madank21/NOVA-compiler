#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

DiagList* diag_list_new(void) {
    DiagList* list = (DiagList*)calloc(1, sizeof(DiagList));
    if (!list) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    list->capacity = 16;
    list->items = (Diag*)malloc(sizeof(Diag) * (size_t)list->capacity);
    if (!list->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    return list;
}

void diag_add(DiagList* list, const char* level, int line, int column, const char* fmt, ...) {
    if (!list) return;
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        Diag* grown = (Diag*)realloc(list->items, sizeof(Diag) * (size_t)list->capacity);
        if (!grown) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        list->items = grown;
    }
    Diag* d = &list->items[list->count++];
    d->level = level;
    d->line = line;
    d->column = column;
    va_list args;
    va_start(args, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, args);
    va_end(args);
}

int diag_has_errors(const DiagList* list) {
    if (!list) return 0;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].level, "error") == 0) return 1;
    }
    return 0;
}

void diag_list_free(DiagList* list) {
    if (!list) return;
    free(list->items);
    free(list);
}
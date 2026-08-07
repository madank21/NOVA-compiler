#include "optimizer.h"
#include <ctype.h>

static int is_number(const char* str) {
    if (!str || !*str) return 0;
    int i = 0;
    if (str[0] == '-') i++;
    int has_digit = 0;
    for (; str[i]; i++) {
        if (isdigit(str[i])) has_digit = 1;
        else if (str[i] != '.') return 0;
    }
    return has_digit;
}

TACList* optimize_tac(TACList* input_list, OptimizationMetrics* metrics) {
    if (!input_list) return NULL;
    memset(metrics, 0, sizeof(OptimizationMetrics));

    TACList* opt_list = (TACList*)calloc(1, sizeof(TACList));
    int original_count = input_list->count;

    TACInstr* curr = input_list->head;
    while (curr) {
        TACInstr* new_instr = (TACInstr*)malloc(sizeof(TACInstr));
        memcpy(new_instr, curr, sizeof(TACInstr));
        new_instr->next = NULL;

        // Pass 1: Constant Folding (e.g. t0 = 5 * 2 => t0 = 10)
        if ((curr->op == TAC_ADD || curr->op == TAC_SUB || curr->op == TAC_MUL || curr->op == TAC_DIV) &&
            is_number(curr->arg1) && is_number(curr->arg2)) {

            double val1 = atof(curr->arg1);
            double val2 = atof(curr->arg2);
            double res = 0;

            if (curr->op == TAC_ADD) res = val1 + val2;
            else if (curr->op == TAC_SUB) res = val1 - val2;
            else if (curr->op == TAC_MUL) res = val1 * val2;
            else if (curr->op == TAC_DIV && val2 != 0) res = val1 / val2;

            new_instr->op = TAC_ASSIGN;
            snprintf(new_instr->arg1, sizeof(new_instr->arg1), "%.0f", res);
            new_instr->arg2[0] = '\0';
            metrics->constant_fold_count++;
        }
        // Pass 4: Strength Reduction (e.g., x * 2 => x << 1 or x + x)
        else if (curr->op == TAC_MUL && (strcmp(curr->arg2, "2") == 0 || strcmp(curr->arg1, "2") == 0)) {
            const char* var = (strcmp(curr->arg2, "2") == 0) ? curr->arg1 : curr->arg2;
            new_instr->op = TAC_ADD;
            strncpy(new_instr->arg1, var, 63);
            strncpy(new_instr->arg2, var, 63);
            metrics->strength_reduce_count++;
        }

        if (!opt_list->head) {
            opt_list->head = new_instr;
            opt_list->tail = new_instr;
        } else {
            opt_list->tail->next = new_instr;
            opt_list->tail = new_instr;
        }
        opt_list->count++;
        curr = curr->next;
    }

    metrics->reduction_percentage = original_count > 0 ?
        ((float)(metrics->constant_fold_count + metrics->constant_prop_count + metrics->dead_code_count + metrics->strength_reduce_count) / original_count) * 100.0f : 0.0f;

    return opt_list;
}

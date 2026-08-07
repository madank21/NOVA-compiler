#include "tac.h"

static TACList* create_tac_list() {
    TACList* list = (TACList*)calloc(1, sizeof(TACList));
    return list;
}

static void emit_tac(TACList* list, TACOpcode op, const char* res, const char* a1, const char* a2, int line) {
    TACInstr* instr = (TACInstr*)calloc(1, sizeof(TACInstr));
    instr->op = op;
    if (res) strncpy(instr->result, res, 63);
    if (a1) strncpy(instr->arg1, a1, 63);
    if (a2) strncpy(instr->arg2, a2, 63);
    instr->line = line;

    if (!list->head) {
        list->head = instr;
        list->tail = instr;
    } else {
        list->tail->next = instr;
        list->tail = instr;
    }
    list->count++;
}

static char* new_temp(TACList* list) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "t%d", list->temp_count++);
    return buf;
}

static char* new_label(TACList* list) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "L%d", list->label_count++);
    return buf;
}

static char* generate_expr_tac(ASTNode* node, TACList* list) {
    if (!node) return "";
    static char res[64];

    if (node->type == NODE_INT_LITERAL) {
        snprintf(res, sizeof(res), "%lld", node->int_val);
        return res;
    }
    if (node->type == NODE_FLOAT_LITERAL) {
        snprintf(res, sizeof(res), "%.2f", node->float_val);
        return res;
    }
    if (node->type == NODE_STRING_LITERAL) {
        snprintf(res, sizeof(res), "\"%s\"", node->string_val);
        return res;
    }
    if (node->type == NODE_IDENTIFIER) {
        return node->identifier;
    }

    if (node->type == NODE_BINARY_OP) {
        char left_reg[64];
        char right_reg[64];
        strncpy(left_reg, generate_expr_tac(node->left, list), 63);
        strncpy(right_reg, generate_expr_tac(node->right, list), 63);

        char* temp = strdup(new_temp(list));
        TACOpcode op = TAC_ADD;
        if (strcmp(node->op, "+") == 0) op = TAC_ADD;
        else if (strcmp(node->op, "-") == 0) op = TAC_SUB;
        else if (strcmp(node->op, "*") == 0) op = TAC_MUL;
        else if (strcmp(node->op, "/") == 0) op = TAC_DIV;
        else if (strcmp(node->op, "%") == 0) op = TAC_MOD;

        emit_tac(list, op, temp, left_reg, right_reg, node->line);
        return temp;
    }

    if (node->type == NODE_ASSIGNMENT) {
        char right_reg[64];
        strncpy(right_reg, generate_expr_tac(node->right, list), 63);
        emit_tac(list, TAC_ASSIGN, node->left->identifier, right_reg, NULL, node->line);
        return node->left->identifier;
    }

    if (node->type == NODE_FUNC_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            char arg_reg[64];
            strncpy(arg_reg, generate_expr_tac(node->children[i], list), 63);
            emit_tac(list, TAC_PARAM, arg_reg, NULL, NULL, node->line);
        }
        if (strcmp(node->identifier, "printf") == 0) {
            emit_tac(list, TAC_PRINT, "printf", NULL, NULL, node->line);
            return "";
        }
        char* temp = strdup(new_temp(list));
        char arg_cnt[16];
        snprintf(arg_cnt, sizeof(arg_cnt), "%d", node->child_count);
        emit_tac(list, TAC_CALL, temp, node->identifier, arg_cnt, node->line);
        return temp;
    }

    return "";
}

static void generate_stmt_tac(ASTNode* node, TACList* list) {
    if (!node) return;

    if (node->type == NODE_FUNCTION_DEF) {
        emit_tac(list, TAC_FUNC_BEGIN, node->identifier, NULL, NULL, node->line);
        if (node->right) generate_stmt_tac(node->right, list);
        emit_tac(list, TAC_FUNC_END, node->identifier, NULL, NULL, node->line);
        return;
    }

    if (node->type == NODE_COMPOUND_STMT) {
        for (int i = 0; i < node->child_count; i++) {
            generate_stmt_tac(node->children[i], list);
        }
        return;
    }

    if (node->type == NODE_VAR_DECL) {
        if (node->left) {
            char val_reg[64];
            strncpy(val_reg, generate_expr_tac(node->left, list), 63);
            emit_tac(list, TAC_ASSIGN, node->identifier, val_reg, NULL, node->line);
        }
        return;
    }

    if (node->type == NODE_IF_STMT) {
        char cond_reg[64];
        strncpy(cond_reg, generate_expr_tac(node->condition, list), 63);
        char* else_label = strdup(new_label(list));
        char* end_label = strdup(new_label(list));

        emit_tac(list, TAC_IF_FALSE, else_label, cond_reg, NULL, node->line);
        generate_stmt_tac(node->left, list);
        emit_tac(list, TAC_GOTO, end_label, NULL, NULL, node->line);
        emit_tac(list, TAC_LABEL, else_label, NULL, NULL, node->line);
        if (node->else_branch) generate_stmt_tac(node->else_branch, list);
        emit_tac(list, TAC_LABEL, end_label, NULL, NULL, node->line);
        return;
    }

    if (node->type == NODE_WHILE_STMT) {
        char* start_label = strdup(new_label(list));
        char* end_label = strdup(new_label(list));

        emit_tac(list, TAC_LABEL, start_label, NULL, NULL, node->line);
        char cond_reg[64];
        strncpy(cond_reg, generate_expr_tac(node->condition, list), 63);
        emit_tac(list, TAC_IF_FALSE, end_label, cond_reg, NULL, node->line);
        generate_stmt_tac(node->left, list);
        emit_tac(list, TAC_GOTO, start_label, NULL, NULL, node->line);
        emit_tac(list, TAC_LABEL, end_label, NULL, NULL, node->line);
        return;
    }

    if (node->type == NODE_RETURN_STMT) {
        if (node->left) {
            char ret_reg[64];
            strncpy(ret_reg, generate_expr_tac(node->left, list), 63);
            emit_tac(list, TAC_RETURN, ret_reg, NULL, NULL, node->line);
        } else {
            emit_tac(list, TAC_RETURN, NULL, NULL, NULL, node->line);
        }
        return;
    }

    if (node->type == NODE_EXPRESSION_STMT) {
        if (node->left) generate_expr_tac(node->left, list);
        return;
    }
}

TACList* generate_tac(ASTNode* root) {
    TACList* list = create_tac_list();
    if (root->type == NODE_PROGRAM) {
        for (int i = 0; i < root->child_count; i++) {
            generate_stmt_tac(root->children[i], list);
        }
    } else {
        generate_stmt_tac(root, list);
    }
    return list;
}

void tac_list_free(TACList* list) {
    if (!list) return;
    TACInstr* curr = list->head;
    while (curr) {
        TACInstr* next = curr->next;
        free(curr);
        curr = next;
    }
    free(list);
}

const char* tac_op_to_string(TACOpcode op) {
    switch (op) {
        case TAC_ADD: return "+";
        case TAC_SUB: return "-";
        case TAC_MUL: return "*";
        case TAC_DIV: return "/";
        case TAC_MOD: return "%";
        case TAC_ASSIGN: return "=";
        case TAC_LABEL: return "LABEL";
        case TAC_GOTO: return "GOTO";
        case TAC_IF_FALSE: return "IF_FALSE";
        case TAC_IF_TRUE: return "IF_TRUE";
        case TAC_PARAM: return "PARAM";
        case TAC_CALL: return "CALL";
        case TAC_RETURN: return "RETURN";
        case TAC_FUNC_BEGIN: return "FUNC_BEGIN";
        case TAC_FUNC_END: return "FUNC_END";
        case TAC_PRINT: return "PRINT";
        default: return "TAC";
    }
}

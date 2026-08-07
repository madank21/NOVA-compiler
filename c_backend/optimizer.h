#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "tac.h"

typedef struct {
    int constant_fold_count;
    int constant_prop_count;
    int dead_code_count;
    int strength_reduce_count;
    float reduction_percentage;
} OptimizationMetrics;

TACList* optimize_tac(TACList* input_list, OptimizationMetrics* metrics);

#endif // OPTIMIZER_H

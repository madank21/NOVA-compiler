#ifndef NOVA_OPTIMIZER_H
#define NOVA_OPTIMIZER_H

#include "tac.h"

typedef struct {
    int constant_fold;
    int constant_prop;
    int dead_code;
    int strength_reduce;
    double reduction_percentage;
} OptimizationMetrics;

TACList* optimize_tac(const TACList* input, OptimizationMetrics* metrics);

#endif

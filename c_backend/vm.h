#ifndef NOVA_VM_H
#define NOVA_VM_H

/* The VM types (VMStep, VMResult) and the nova_vm_run / nova_vm_free
 * interface are defined in compile.h, the single source of truth for the
 * native backend's data structures. This header is kept as an alias so
 * `#include "vm.h"` continues to work. */
#include "compile.h"

#endif

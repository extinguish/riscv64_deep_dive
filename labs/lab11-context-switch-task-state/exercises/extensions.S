#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 3: calculate aligned RVV task-context storage. */
.globl lab_vector_context_size
.type lab_vector_context_size, @function
lab_vector_context_size:
    li a0, -1
    ret
.size lab_vector_context_size, .-lab_vector_context_size
/* TODO 4: derive save/restore/disable actions for FS/VS state. */
.globl lab_extension_switch_plan
.type lab_extension_switch_plan, @function
lab_extension_switch_plan:
    li a0, -1
    ret
.size lab_extension_switch_plan, .-lab_extension_switch_plan

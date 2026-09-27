#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 1: discover an M/S PLIC context from a sparse firmware table. */
.globl lab_plic_context_lookup
.type lab_plic_context_lookup, @function
lab_plic_context_lookup:
    li      a0, -1
    ret
.size lab_plic_context_lookup, .-lab_plic_context_lookup

#include "lab_layout.h"

.section .reloc_image, "ax", @progbits
.align 3

/*
 * TODO 3: return (a0 ^ internal_constant) + LAB_RELOC_ADDEND.
 * Access the internal constant with a position-independent reference.
 */
.globl lab_relocated_stage
.type lab_relocated_stage, @function
lab_relocated_stage:
    li      a0, -1
    ret
.size lab_relocated_stage, .-lab_relocated_stage

.align 3
.globl lab_reloc_internal_constant
lab_reloc_internal_constant:
    .dword LAB_RELOC_INTERNAL_VALUE

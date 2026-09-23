#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3: int lab_sv39_translate(root, va, access, output_pa) */
.globl lab_sv39_translate
.type lab_sv39_translate, @function
lab_sv39_translate:
    li      a0, LAB_WALK_ERR_INVALID
    ret
.size lab_sv39_translate, .-lab_sv39_translate

#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1a: encode one lab-format IO leaf PTE. */
.globl lab_iopte_make
.type lab_iopte_make, @function
lab_iopte_make:
    li      a0, -1
    ret
.size lab_iopte_make, .-lab_iopte_make

/* TODO 1b: validate and decode one lab-format IO leaf PTE. */
.globl lab_iopte_decode
.type lab_iopte_decode, @function
lab_iopte_decode:
    li      a0, 0
    ret
.size lab_iopte_decode, .-lab_iopte_decode

#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 2a: uint64_t lab_make_table_pte(uint64_t pa) */
.globl lab_make_table_pte
.type lab_make_table_pte, @function
lab_make_table_pte:
    li      a0, 0
    ret
.size lab_make_table_pte, .-lab_make_table_pte

/* TODO 2b: uint64_t lab_make_leaf_pte(uint64_t pa, uint64_t flags) */
.globl lab_make_leaf_pte
.type lab_make_leaf_pte, @function
lab_make_leaf_pte:
    li      a0, 0
    ret
.size lab_make_leaf_pte, .-lab_make_leaf_pte

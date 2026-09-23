#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3: int lab_pmp_match(addrs, config, count, addr, size, access) */
.globl lab_pmp_match
.type lab_pmp_match, @function
lab_pmp_match:
    li      a0, 0
    ret
.size lab_pmp_match, .-lab_pmp_match

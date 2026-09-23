#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4a: int lab_install_4k(root, va, pa, flags) */
.globl lab_install_4k
.type lab_install_4k, @function
lab_install_4k:
    li      a0, 0
    ret
.size lab_install_4k, .-lab_install_4k

/* TODO 4b: int lab_unmap_4k(root, va) */
.globl lab_unmap_4k
.type lab_unmap_4k, @function
lab_unmap_4k:
    li      a0, 0
    ret
.size lab_unmap_4k, .-lab_unmap_4k

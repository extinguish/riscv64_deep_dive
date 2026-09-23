#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4a: int lab_pmp_program_window(base, size, permissions) */
.globl lab_pmp_program_window
.type lab_pmp_program_window, @function
lab_pmp_program_window:
    li      a0, 0
    ret
.size lab_pmp_program_window, .-lab_pmp_program_window

/* TODO 4b: int lab_pmp_disable_window(void) */
.globl lab_pmp_disable_window
.type lab_pmp_disable_window, @function
lab_pmp_disable_window:
    li      a0, 0
    ret
.size lab_pmp_disable_window, .-lab_pmp_disable_window

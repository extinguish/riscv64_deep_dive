#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 7: select the earliest absolute deadline and clamp overdue work to now. */
.globl lab_timer_select
.type lab_timer_select, @function
lab_timer_select:
    li      a0, -1
    ret
.size lab_timer_select, .-lab_timer_select

/* TODO 8: validate and program one RV64 mtimecmp-style 64-bit register. */
.globl lab_timer_program
.type lab_timer_program, @function
lab_timer_program:
    li      a0, 0
    ret
.size lab_timer_program, .-lab_timer_program

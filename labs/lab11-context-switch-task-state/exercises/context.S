#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 1: build a synthetic first-run task context. */
.globl lab_context_init
.type lab_context_init, @function
lab_context_init:
    li a0, 0
    ret
.size lab_context_init, .-lab_context_init
/* TODO 2: save and restore the cooperative switch register set. */
.globl lab_context_switch
.type lab_context_switch, @function
lab_context_switch:
    ret
.size lab_context_switch, .-lab_context_switch

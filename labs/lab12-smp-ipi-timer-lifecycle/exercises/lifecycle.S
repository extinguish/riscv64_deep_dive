#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 3: enforce the OFFLINE/PENDING/ONLINE lifecycle state machine. */
.globl lab_cpu_transition
.type lab_cpu_transition, @function
lab_cpu_transition:
    li      a0, -1
    ret
.size lab_cpu_transition, .-lab_cpu_transition

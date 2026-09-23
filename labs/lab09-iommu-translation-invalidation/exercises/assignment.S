#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4a: enforce the assignment lifecycle state machine. */
.globl lab_assignment_transition
.type lab_assignment_transition, @function
lab_assignment_transition:
    li      a0, 0
    ret
.size lab_assignment_transition, .-lab_assignment_transition

/* TODO 4b: authorize DMA/MSI only within the active assignment. */
.globl lab_assignment_authorize
.type lab_assignment_authorize, @function
lab_assignment_authorize:
    li      a0, 0
    ret
.size lab_assignment_authorize, .-lab_assignment_authorize

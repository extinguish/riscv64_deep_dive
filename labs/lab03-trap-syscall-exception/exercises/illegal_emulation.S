#include "lab_constants.h"
#include "lab_trap_frame.h"

.section .text.exercise
.align 2

/* TODO 3: long lab_emulate_instruction(struct lab_trap_frame *frame) */
.globl lab_emulate_instruction
.type lab_emulate_instruction, @function
lab_emulate_instruction:
    li      a0, -1
    ret
.size lab_emulate_instruction, .-lab_emulate_instruction

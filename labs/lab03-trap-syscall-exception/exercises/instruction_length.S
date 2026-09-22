.section .text.exercise
.align 2

/* TODO 1: unsigned long lab_instruction_length(const void *address) */
.globl lab_instruction_length
.type lab_instruction_length, @function
lab_instruction_length:
    li      a0, 0
    ret
.size lab_instruction_length, .-lab_instruction_length

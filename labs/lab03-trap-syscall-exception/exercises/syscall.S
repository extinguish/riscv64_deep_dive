#include "lab_trap_frame.h"

.section .text.exercise
.align 2

/* TODO 2: long lab_handle_syscall(struct lab_trap_frame *frame) */
.globl lab_handle_syscall
.type lab_handle_syscall, @function
lab_handle_syscall:
    li      a0, -1
    ret
.size lab_handle_syscall, .-lab_handle_syscall

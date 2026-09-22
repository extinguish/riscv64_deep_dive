#include "lab_trap_frame.h"

.section .text.trap.exercise
.align 2

/*
 * TODO 4: implement the U-to-S entry/exit path described in README.md.
 * sscratch contains the S trap-stack top when U-mode is running.
 */
.globl lab_s_trap_entry
.type lab_s_trap_entry, @function
lab_s_trap_entry:
    lla     a0, .Ltodo_message
    call    lab_early_puts
    li      a0, 1
    tail    lab_early_exit
.size lab_s_trap_entry, .-lab_s_trap_entry

.section .rodata
.Ltodo_message:
    .asciz "LAB03 TODO: lab_s_trap_entry is not implemented\n"

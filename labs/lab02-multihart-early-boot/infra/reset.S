#include "lab_layout.h"

.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    csrci   mstatus, 8
    csrw    mie, zero

    csrr    a0, mhartid
    li      t0, LAB_HART_COUNT
    bgeu    a0, t0, .Lunknown_hart

    lla     sp, __emergency_stacks_start
    slli    t0, a0, 9
    add     sp, sp, t0
    addi    sp, sp, LAB_EMERGENCY_STACK_SIZE
    andi    sp, sp, -16

    lla     t0, lab_early_trap
    csrw    mtvec, t0
    tail    lab_boot_entry

.Lunknown_hart:
    wfi
    j       .Lunknown_hart
.size _start, .-_start


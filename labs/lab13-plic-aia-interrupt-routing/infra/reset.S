#include "lab_layout.h"
.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    csrci   mstatus, 8
    csrw    mie, zero
    csrr    a0, mhartid
    bnez    a0, .Lpark
    lla     sp, __stack_top
    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop
    lla     t0, lab_trap_entry
    csrw    mtvec, t0
    lla     t0, __bss_start
    lla     t1, __bss_end
.Lclear_bss:
    bgeu    t0, t1, .Lbss_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .Lclear_bss
.Lbss_done:
    tail    lab_hart_entry
.Lpark:
    wfi
    j       .Lpark
.size _start, .-_start

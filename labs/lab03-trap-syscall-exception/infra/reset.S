#include "lab_constants.h"

.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop
    lla     sp, __m_stack_top

    lla     t0, lab_m_fatal_trap
    csrw    mtvec, t0
    csrci   mstatus, 8
    csrw    mie, zero

    lla     t0, __bss_start
    lla     t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x0f
    csrw    pmpcfg0, t0

    li      t0, ((1 << LAB_SCAUSE_ILLEGAL_INSTRUCTION) | \
                 (1 << LAB_SCAUSE_BREAKPOINT) | \
                 (1 << LAB_SCAUSE_USER_ECALL))
    csrw    medeleg, t0
    csrw    mideleg, zero

    lla     t0, lab_s_kernel_entry
    csrw    mepc, t0
    csrr    t0, mstatus
    li      t1, ~(3 << 11)
    and     t0, t0, t1
    li      t1, (1 << 11)
    or      t0, t0, t1
    csrw    mstatus, t0
    mret
.size _start, .-_start

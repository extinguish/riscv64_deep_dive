#include "lab_layout.h"

.section .text.flow
.align 2
.globl lab_hart_entry
.type lab_hart_entry, @function
lab_hart_entry:
    lla     t0, lab_m_trap_vector
    csrw    mtvec, t0
    csrw    medeleg, zero
    csrw    mideleg, zero

    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    li      t0, (1 << LAB_CAUSE_BREAKPOINT) | (1 << LAB_CAUSE_USER_ECALL)
    csrw    medeleg, t0
    csrw    satp, zero
    sfence.vma zero, zero

    csrr    t0, mstatus
    li      t1, ~(3 << 11)
    and     t0, t0, t1
    li      t1, (1 << 11) | (1 << 7)
    or      t0, t0, t1
    csrw    mstatus, t0
    lla     t0, lab_full_main
    csrw    mepc, t0
    mret
.size lab_hart_entry, .-lab_hart_entry

.section .text
.align 2
.globl lab_enter_supervisor
.type lab_enter_supervisor, @function
lab_enter_supervisor:
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    li      t0, 0xb000
    csrw    medeleg, t0
    csrw    stvec, a2
    csrw    satp, a1
    sfence.vma x0, x0

    csrr    t0, mstatus
    li      t1, -6145
    and     t0, t0, t1
    li      t1, 0x800
    or      t0, t0, t1
    csrw    mstatus, t0
    csrw    mepc, a0
    mret
.size lab_enter_supervisor, .-lab_enter_supervisor

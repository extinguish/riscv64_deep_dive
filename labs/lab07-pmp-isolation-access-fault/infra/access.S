.section .text
.align 2
.globl lab_try_mprv_load64
.type lab_try_mprv_load64, @function
lab_try_mprv_load64:
    addi    sp, sp, -16
    sd      s0, 0(sp)
    csrr    s0, mstatus
    li      t0, -137217
    and     t0, s0, t0
    li      t1, 0x20800
    or      t0, t0, t1
    csrw    mstatus, t0
.globl lab_mprv_load_fault
lab_mprv_load_fault:
    ld      t2, (a0)
    csrw    mstatus, s0
    sd      t2, (a1)
    li      a0, 1
    j       .Lload_return
.globl lab_mprv_load_recover
lab_mprv_load_recover:
    csrw    mstatus, s0
    li      a0, 0
.Lload_return:
    ld      s0, 0(sp)
    addi    sp, sp, 16
    ret
.size lab_try_mprv_load64, .-lab_try_mprv_load64

.globl lab_try_mprv_store64
.type lab_try_mprv_store64, @function
lab_try_mprv_store64:
    addi    sp, sp, -16
    sd      s0, 0(sp)
    csrr    s0, mstatus
    li      t0, -137217
    and     t0, s0, t0
    li      t1, 0x20800
    or      t0, t0, t1
    csrw    mstatus, t0
.globl lab_mprv_store_fault
lab_mprv_store_fault:
    sd      a1, (a0)
    csrw    mstatus, s0
    li      a0, 1
    j       .Lstore_return
.globl lab_mprv_store_recover
lab_mprv_store_recover:
    csrw    mstatus, s0
    li      a0, 0
.Lstore_return:
    ld      s0, 0(sp)
    addi    sp, sp, 16
    ret
.size lab_try_mprv_store64, .-lab_try_mprv_store64

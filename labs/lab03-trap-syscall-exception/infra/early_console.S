.section .text
.align 2

.globl lab_early_puts
.type lab_early_puts, @function
lab_early_puts:
    li      t0, 0x10000000
1:
    lbu     t1, 0(a0)
    beqz    t1, 4f
    li      t2, 10
    bne     t1, t2, 2f
3:
    lbu     t3, 5(t0)
    andi    t3, t3, 0x20
    beqz    t3, 3b
    li      t3, 13
    sb      t3, 0(t0)
2:
    lbu     t3, 5(t0)
    andi    t3, t3, 0x20
    beqz    t3, 2b
    sb      t1, 0(t0)
    addi    a0, a0, 1
    j       1b
4:
    ret
.size lab_early_puts, .-lab_early_puts

.globl lab_early_exit
.type lab_early_exit, @function
lab_early_exit:
    beqz    a0, 1f
    li      t0, 0x13333
    j       2f
1:
    li      t0, 0x5555
2:
    li      t1, 0x100000
    sw      t0, 0(t1)
3:
    wfi
    j       3b
.size lab_early_exit, .-lab_early_exit

.globl lab_m_fatal_trap
.type lab_m_fatal_trap, @function
lab_m_fatal_trap:
    lla     a0, .Lm_fatal
    call    lab_early_puts
    li      a0, 1
    tail    lab_early_exit
.size lab_m_fatal_trap, .-lab_m_fatal_trap

.section .rodata
.Lm_fatal:
    .asciz "[FATAL] Trap escalated to M-mode\n"

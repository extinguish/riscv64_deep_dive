.section .text
.align 2
.globl lab_early_puts
.type lab_early_puts, @function
lab_early_puts:
    li      t0, 0x10000000
1:
    lbu     t1, 0(a0)
    beqz    t1, 3f
2:
    lbu     t2, 5(t0)
    andi    t2, t2, 0x20
    beqz    t2, 2b
    sb      t1, 0(t0)
    addi    a0, a0, 1
    j       1b
3:
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

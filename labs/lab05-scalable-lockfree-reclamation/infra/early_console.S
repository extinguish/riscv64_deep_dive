.equ UART_BASE, 0x10000000
.equ TEST_FINISHER, 0x100000

.section .text
.align 2
.globl lab_early_putc
.type lab_early_putc, @function
lab_early_putc:
    li      t0, UART_BASE
1:
    lbu     t1, 5(t0)
    andi    t1, t1, 0x20
    beqz    t1, 1b
    sb      a0, 0(t0)
    ret
.size lab_early_putc, .-lab_early_putc

.globl lab_early_puts
.type lab_early_puts, @function
lab_early_puts:
    addi    sp, sp, -16
    sd      ra, 8(sp)
    sd      s0, 0(sp)
    mv      s0, a0
1:
    lbu     a0, 0(s0)
    beqz    a0, 2f
    call    lab_early_putc
    addi    s0, s0, 1
    j       1b
2:
    ld      s0, 0(sp)
    ld      ra, 8(sp)
    addi    sp, sp, 16
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
    li      t1, TEST_FINISHER
    sw      t0, 0(t1)
3:
    wfi
    j       3b
.size lab_early_exit, .-lab_early_exit

.section .text
.align 2
.globl memset
.type memset, @function
memset:
    mv      t0, a0
    beqz    a2, 2f
1:
    sb      a1, 0(t0)
    addi    t0, t0, 1
    addi    a2, a2, -1
    bnez    a2, 1b
2:
    ret
.size memset, .-memset

.globl __memset
.set __memset, memset

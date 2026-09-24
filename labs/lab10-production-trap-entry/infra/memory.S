.section .text
.align 2
.globl lab_memzero
.type lab_memzero, @function
lab_memzero:
    mv      t0, a0
    beqz    a1, .Lmemzero_done
.Lmemzero_loop:
    sb      zero, 0(t0)
    addi    t0, t0, 1
    addi    a1, a1, -1
    bnez    a1, .Lmemzero_loop
.Lmemzero_done:
    ret
.size lab_memzero, .-lab_memzero

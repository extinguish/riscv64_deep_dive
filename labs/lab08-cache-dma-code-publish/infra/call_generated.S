.section .text
.align 2
.globl lab_call_generated
.type lab_call_generated, @function
lab_call_generated:
    addi    sp, sp, -16
    sd      ra, 8(sp)
    jalr    ra, a0, 0
    ld      ra, 8(sp)
    addi    sp, sp, 16
    ret
.size lab_call_generated, .-lab_call_generated

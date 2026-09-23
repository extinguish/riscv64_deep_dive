.section .text
.align 2
.globl lab_try_load64
.type lab_try_load64, @function
lab_try_load64:
    li      a2, 0
.globl lab_try_load64_fault
lab_try_load64_fault:
    ld      t0, (a0)
    sd      t0, (a1)
    li      a0, 1
    ret
.globl lab_try_load64_recover
lab_try_load64_recover:
    li      a0, 0
    ret
.size lab_try_load64, .-lab_try_load64

.globl lab_try_store64
.type lab_try_store64, @function
lab_try_store64:
    li      a2, 0
.globl lab_try_store64_fault
lab_try_store64_fault:
    sd      a1, (a0)
    li      a0, 1
    ret
.globl lab_try_store64_recover
lab_try_store64_recover:
    li      a0, 0
    ret
.size lab_try_store64, .-lab_try_store64

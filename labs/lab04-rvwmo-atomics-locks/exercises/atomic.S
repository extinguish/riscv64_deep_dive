.section .text.exercise
.align 2

/* TODO 1: uint64_t lab_atomic_fetch_add(uint64_t *address, uint64_t addend) */
.globl lab_atomic_fetch_add
.type lab_atomic_fetch_add, @function
lab_atomic_fetch_add:
    li      a0, -1
    ret
.size lab_atomic_fetch_add, .-lab_atomic_fetch_add

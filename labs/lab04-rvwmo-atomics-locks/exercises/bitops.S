.section .text.exercise
.align 2

/* TODO 5: uint64_t lab_atomic_test_and_set_bit(word, bit_index) */
.globl lab_atomic_test_and_set_bit
.type lab_atomic_test_and_set_bit, @function
lab_atomic_test_and_set_bit:
    li      a0, -1
    ret
.size lab_atomic_test_and_set_bit, .-lab_atomic_test_and_set_bit

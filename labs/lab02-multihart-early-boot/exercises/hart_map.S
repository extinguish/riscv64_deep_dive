.section .text.exercise
.align 2

/* TODO 1: long lab_hartid_to_index(hartid, table, count) */
.globl lab_hartid_to_index
.type lab_hartid_to_index, @function
lab_hartid_to_index:
    li      a0, -1
    ret
.size lab_hartid_to_index, .-lab_hartid_to_index

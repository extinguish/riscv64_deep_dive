#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 1: map a possibly sparse physical hart ID to a logical CPU ID. */
.globl lab_hartid_to_cpu
.type lab_hartid_to_cpu, @function
lab_hartid_to_cpu:
    li      a0, -1
    ret
.size lab_hartid_to_cpu, .-lab_hartid_to_cpu

/* TODO 2: construct one SBI hart_mask window for sparse physical IDs. */
.globl lab_hart_mask_chunk
.type lab_hart_mask_chunk, @function
lab_hart_mask_chunk:
    li      a0, -1
    ret
.size lab_hart_mask_chunk, .-lab_hart_mask_chunk

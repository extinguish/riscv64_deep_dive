#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 4: release-publish coalesced reason bits and return the old word. */
.globl lab_ipi_publish
.type lab_ipi_publish, @function
lab_ipi_publish:
    li      a0, -1
    ret
.size lab_ipi_publish, .-lab_ipi_publish

/* TODO 5: atomically acquire and clear all pending reason bits. */
.globl lab_ipi_consume
.type lab_ipi_consume, @function
lab_ipi_consume:
    li      a0, -1
    ret
.size lab_ipi_consume, .-lab_ipi_consume

/* TODO 6: use check -> WFI -> recheck; WFI may return spuriously. */
.globl lab_wait_for_bits
.type lab_wait_for_bits, @function
lab_wait_for_bits:
    li      a0, 0
    ret
.size lab_wait_for_bits, .-lab_wait_for_bits

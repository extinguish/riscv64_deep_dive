#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1: normalize [address, address + size) to cache-block boundaries. */
.globl lab_normalize_cmo_range
.type lab_normalize_cmo_range, @function
lab_normalize_cmo_range:
    li      a0, 0
    ret
.size lab_normalize_cmo_range, .-lab_normalize_cmo_range

/* TODO 2: validate the operation/policy and emit one event per block. */
.globl lab_cmo_apply
.type lab_cmo_apply, @function
lab_cmo_apply:
    li      a0, 0
    ret
.size lab_cmo_apply, .-lab_cmo_apply

#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1a: uint64_t lab_pmp_napot_encode(base, size) */
.globl lab_pmp_napot_encode
.type lab_pmp_napot_encode, @function
lab_pmp_napot_encode:
    li      a0, -1
    ret
.size lab_pmp_napot_encode, .-lab_pmp_napot_encode

/* TODO 1b: int lab_pmp_napot_decode(pmpaddr, base_out, size_out) */
.globl lab_pmp_napot_decode
.type lab_pmp_napot_decode, @function
lab_pmp_napot_decode:
    li      a0, 0
    ret
.size lab_pmp_napot_decode, .-lab_pmp_napot_decode

/* TODO 1c: uint64_t lab_pmp_tor_encode(top) */
.globl lab_pmp_tor_encode
.type lab_pmp_tor_encode, @function
lab_pmp_tor_encode:
    li      a0, -1
    ret
.size lab_pmp_tor_encode, .-lab_pmp_tor_encode

/* TODO 2: uint64_t lab_pmp_make_cfg(perms, mode, locked) */
.globl lab_pmp_make_cfg
.type lab_pmp_make_cfg, @function
lab_pmp_make_cfg:
    li      a0, -1
    ret
.size lab_pmp_make_cfg, .-lab_pmp_make_cfg

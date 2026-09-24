#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 2a: safely split scause and apply the matching table bound. */
.globl lab_decode_scause
.type lab_decode_scause, @function
lab_decode_scause:
    li      a0, 0
    ret
.size lab_decode_scause, .-lab_decode_scause

/* TODO 2b: advance EPC for supported 16/32-bit instruction encodings. */
.globl lab_advance_epc
.type lab_advance_epc, @function
lab_advance_epc:
    li      a0, -1
    ret
.size lab_advance_epc, .-lab_advance_epc

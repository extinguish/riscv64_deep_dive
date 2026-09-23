#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 2a: int lab_spsc_try_push(ring, value) */
.globl lab_spsc_try_push
.type lab_spsc_try_push, @function
lab_spsc_try_push:
    li      a0, 0
    ret
.size lab_spsc_try_push, .-lab_spsc_try_push

/* TODO 2b: int lab_spsc_try_pop(ring, output) */
.globl lab_spsc_try_pop
.type lab_spsc_try_pop, @function
lab_spsc_try_pop:
    li      a0, 0
    ret
.size lab_spsc_try_pop, .-lab_spsc_try_pop

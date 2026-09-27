#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 2: model PLIC enable/priority/threshold arbitration. */
.globl lab_plic_select
.type lab_plic_select, @function
lab_plic_select:
    li      a0, -1
    ret
.size lab_plic_select, .-lab_plic_select

/* TODO 3: claim, acknowledge the device, then complete a source. */
.globl lab_plic_service
.type lab_plic_service, @function
lab_plic_service:
    li      a0, -1
    ret
.size lab_plic_service, .-lab_plic_service

/* TODO 4: migrate one source only after the old handler is quiescent. */
.globl lab_plic_retarget
.type lab_plic_retarget, @function
lab_plic_retarget:
    li      a0, 0
    ret
.size lab_plic_retarget, .-lab_plic_retarget

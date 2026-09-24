#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1a: model S-mode trap entry SIE/SPIE/SPP transitions. */
.globl lab_status_on_trap
.type lab_status_on_trap, @function
lab_status_on_trap:
    li      a0, -1
    ret
.size lab_status_on_trap, .-lab_status_on_trap

/* TODO 1b: model SRET SIE/SPIE/SPP transitions. */
.globl lab_status_on_sret
.type lab_status_on_sret, @function
lab_status_on_sret:
    li      a0, 0
    ret
.size lab_status_on_sret, .-lab_status_on_sret

/* TODO 1c: sanitize a candidate sstatus for return to U-mode. */
.globl lab_sanitize_user_sstatus
.type lab_sanitize_user_sstatus, @function
lab_sanitize_user_sstatus:
    li      a0, 0
    ret
.size lab_sanitize_user_sstatus, .-lab_sanitize_user_sstatus

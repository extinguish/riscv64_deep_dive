#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 5a: uint64_t lab_qsbr_start(domain) */
.globl lab_qsbr_start
.type lab_qsbr_start, @function
lab_qsbr_start:
    li      a0, 0
    ret
.size lab_qsbr_start, .-lab_qsbr_start

/* TODO 5b: void lab_qsbr_report(domain, hartid) */
.globl lab_qsbr_report
.type lab_qsbr_report, @function
lab_qsbr_report:
    ret
.size lab_qsbr_report, .-lab_qsbr_report

/* TODO 5c: int lab_qsbr_complete(domain, target_epoch) */
.globl lab_qsbr_complete
.type lab_qsbr_complete, @function
lab_qsbr_complete:
    li      a0, 0
    ret
.size lab_qsbr_complete, .-lab_qsbr_complete

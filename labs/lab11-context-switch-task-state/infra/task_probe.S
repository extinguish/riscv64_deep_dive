#include "lab_layout.h"
.section .text.flow
.align 2
.globl lab_task_probe
.type lab_task_probe, @function
lab_task_probe:
    li      t0, LAB_TASK_ARGUMENT
    beq     a0, t0, 1f
    lla     t0, lab_task_probe_failed
    li      t1, 1
    sd      t1, 0(t0)
1:
    li      s3, LAB_TASK_S3_SENTINEL
    li      s11, LAB_TASK_S11_SENTINEL
    li      t0, LAB_TASK_TP
    beq     tp, t0, 2f
    lla     t0, lab_task_probe_failed
    li      t1, 1
    sd      t1, 0(t0)
2:
    lla     t0, lab_task_phase
    li      t1, 1
    sd      t1, 0(t0)
    addi    sp, sp, -16
    sd      ra, 8(sp)
    lla     a0, lab_task_context
    lla     a1, lab_scheduler_context
    call    lab_context_switch

    li      t0, LAB_TASK_S3_SENTINEL
    bne     s3, t0, .Ltask_bad_resume
    li      t0, LAB_TASK_S11_SENTINEL
    bne     s11, t0, .Ltask_bad_resume
    li      t0, LAB_TASK_TP
    beq     tp, t0, .Ltask_resume_ok
.Ltask_bad_resume:
    lla     t0, lab_task_probe_failed
    li      t1, 1
    sd      t1, 0(t0)
.Ltask_resume_ok:
    lla     t0, lab_task_phase
    li      t1, 2
    sd      t1, 0(t0)
    li      a0, LAB_TASK_RESULT
    ld      ra, 8(sp)
    addi    sp, sp, 16
    ret
.size lab_task_probe, .-lab_task_probe

.globl lab_task_exit
.type lab_task_exit, @function
lab_task_exit:
    lla     t0, lab_task_result
    sd      a0, 0(t0)
    lla     t0, lab_task_context
    li      t1, LAB_CTX_EXITED
    sd      t1, LAB_CTX_STATE(t0)
    mv      a0, t0
    lla     a1, lab_scheduler_context
    call    lab_context_switch
    tail    lab_task_unexpected_return
.size lab_task_exit, .-lab_task_exit

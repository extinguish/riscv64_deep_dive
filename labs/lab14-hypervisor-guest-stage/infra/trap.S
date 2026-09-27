#include "lab_layout.h"
.section .text.trap
.align 2
.globl lab_hs_trap
lab_hs_trap:
    csrrw sp, sscratch, sp
    addi sp, sp, -LAB_FRAME_SIZE
    sd ra, LAB_FRAME_RA(sp)
    sd gp, LAB_FRAME_GP(sp)
    sd tp, LAB_FRAME_TP(sp)
    sd t0, LAB_FRAME_T0(sp)
    sd t1, LAB_FRAME_T1(sp)
    sd t2, LAB_FRAME_T2(sp)
    sd s0, LAB_FRAME_S0(sp)
    sd s1, LAB_FRAME_S1(sp)
    sd a0, LAB_FRAME_A0(sp)
    sd a1, LAB_FRAME_A1(sp)
    sd a2, LAB_FRAME_A2(sp)
    sd a3, LAB_FRAME_A3(sp)
    sd a4, LAB_FRAME_A4(sp)
    sd a5, LAB_FRAME_A5(sp)
    sd a6, LAB_FRAME_A6(sp)
    sd a7, LAB_FRAME_A7(sp)
    sd s2, LAB_FRAME_S2(sp)
    sd s3, LAB_FRAME_S3(sp)
    sd s4, LAB_FRAME_S4(sp)
    sd s5, LAB_FRAME_S5(sp)
    sd s6, LAB_FRAME_S6(sp)
    sd s7, LAB_FRAME_S7(sp)
    sd s8, LAB_FRAME_S8(sp)
    sd s9, LAB_FRAME_S9(sp)
    sd s10, LAB_FRAME_S10(sp)
    sd s11, LAB_FRAME_S11(sp)
    sd t3, LAB_FRAME_T3(sp)
    sd t4, LAB_FRAME_T4(sp)
    sd t5, LAB_FRAME_T5(sp)
    sd t6, LAB_FRAME_T6(sp)
    csrr t0, sscratch
    sd t0, LAB_FRAME_GUEST_SP(sp)
    csrr t0, scause
    sd t0, LAB_FRAME_SCAUSE(sp)
    csrr t0, sepc
    sd t0, LAB_FRAME_SEPC(sp)
    csrr t0, hstatus
    sd t0, LAB_FRAME_HSTATUS(sp)
    mv a0, sp
    call lab_trap_dispatch
    j lab_bad_return

.globl lab_trap_resume
lab_trap_resume:
    mv sp, a0
    ld ra, LAB_FRAME_RA(sp)
    ld gp, LAB_FRAME_GP(sp)
    ld tp, LAB_FRAME_TP(sp)
    ld t0, LAB_FRAME_T0(sp)
    ld t1, LAB_FRAME_T1(sp)
    ld t2, LAB_FRAME_T2(sp)
    ld s0, LAB_FRAME_S0(sp)
    ld s1, LAB_FRAME_S1(sp)
    ld a0, LAB_FRAME_A0(sp)
    ld a1, LAB_FRAME_A1(sp)
    ld a2, LAB_FRAME_A2(sp)
    ld a3, LAB_FRAME_A3(sp)
    ld a4, LAB_FRAME_A4(sp)
    ld a5, LAB_FRAME_A5(sp)
    ld a6, LAB_FRAME_A6(sp)
    ld a7, LAB_FRAME_A7(sp)
    ld s2, LAB_FRAME_S2(sp)
    ld s3, LAB_FRAME_S3(sp)
    ld s4, LAB_FRAME_S4(sp)
    ld s5, LAB_FRAME_S5(sp)
    ld s6, LAB_FRAME_S6(sp)
    ld s7, LAB_FRAME_S7(sp)
    ld s8, LAB_FRAME_S8(sp)
    ld s9, LAB_FRAME_S9(sp)
    ld s10, LAB_FRAME_S10(sp)
    ld s11, LAB_FRAME_S11(sp)
    ld t3, LAB_FRAME_T3(sp)
    ld t4, LAB_FRAME_T4(sp)
    ld t5, LAB_FRAME_T5(sp)
    ld t6, LAB_FRAME_T6(sp)
    addi sp, sp, LAB_FRAME_SIZE
    csrrw sp, sscratch, sp
    sret

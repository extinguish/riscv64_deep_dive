#include "lab_layout.h"
.section .text.trap
.align 2
.globl lab_m_trap
lab_m_trap:
    csrrw sp, mscratch, sp
    addi sp, sp, -LAB_FRAME_SIZE
    sd ra, F_RA(sp)
    sd gp, F_GP(sp)
    sd tp, F_TP(sp)
    sd t0, F_T0(sp)
    sd t1, F_T1(sp)
    sd t2, F_T2(sp)
    sd s0, F_S0(sp)
    sd s1, F_S1(sp)
    sd a0, F_A0(sp)
    sd a1, F_A1(sp)
    sd a2, F_A2(sp)
    sd a3, F_A3(sp)
    sd a4, F_A4(sp)
    sd a5, F_A5(sp)
    sd a6, F_A6(sp)
    sd a7, F_A7(sp)
    sd s2, F_S2(sp)
    sd s3, F_S3(sp)
    sd s4, F_S4(sp)
    sd s5, F_S5(sp)
    sd s6, F_S6(sp)
    sd s7, F_S7(sp)
    sd s8, F_S8(sp)
    sd s9, F_S9(sp)
    sd s10, F_S10(sp)
    sd s11, F_S11(sp)
    sd t3, F_T3(sp)
    sd t4, F_T4(sp)
    sd t5, F_T5(sp)
    sd t6, F_T6(sp)
    csrr t0, mscratch
    sd t0, F_S_SP(sp)
    csrr t0, mcause
    sd t0, F_MCAUSE(sp)
    csrr t0, mepc
    sd t0, F_MEPC(sp)
    csrr t0, mtval
    sd t0, F_MTVAL(sp)
    mv a0, sp
    call lab_trap_dispatch
    ld ra, F_RA(sp)
    ld gp, F_GP(sp)
    ld tp, F_TP(sp)
    ld t0, F_T0(sp)
    ld t1, F_T1(sp)
    ld t2, F_T2(sp)
    ld s0, F_S0(sp)
    ld s1, F_S1(sp)
    ld a0, F_A0(sp)
    ld a1, F_A1(sp)
    ld a2, F_A2(sp)
    ld a3, F_A3(sp)
    ld a4, F_A4(sp)
    ld a5, F_A5(sp)
    ld a6, F_A6(sp)
    ld a7, F_A7(sp)
    ld s2, F_S2(sp)
    ld s3, F_S3(sp)
    ld s4, F_S4(sp)
    ld s5, F_S5(sp)
    ld s6, F_S6(sp)
    ld s7, F_S7(sp)
    ld s8, F_S8(sp)
    ld s9, F_S9(sp)
    ld s10, F_S10(sp)
    ld s11, F_S11(sp)
    ld t3, F_T3(sp)
    ld t4, F_T4(sp)
    ld t5, F_T5(sp)
    ld t6, F_T6(sp)
    addi sp, sp, LAB_FRAME_SIZE
    csrrw sp, mscratch, sp
    mret

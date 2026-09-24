#include "lab_layout.h"

.section .text.user
.align 2
.option push
.option norvc
.globl lab_user_probe
.type lab_user_probe, @function
lab_user_probe:
    li      ra, LAB_SENTINEL_RA
    li      gp, LAB_SENTINEL_GP
    li      tp, LAB_SENTINEL_TP
    li      t0, LAB_SENTINEL_T0
    li      t1, LAB_SENTINEL_T1
    li      t2, LAB_SENTINEL_T2
    li      s0, LAB_SENTINEL_S0
    li      s1, LAB_SENTINEL_S1
    li      s2, 0x820
    li      s3, 0x830
    li      s4, 0x840
    li      s5, 0x850
    li      s6, 0x860
    li      s7, 0x870
    li      s8, 0x880
    li      s9, 0x890
    li      s10, 0x8a0
    li      s11, LAB_SENTINEL_S11
    li      t3, 0x530
    li      t4, 0x540
    li      t5, 0x550
    li      t6, 0x560
    li      a0, LAB_SENTINEL_A0
    li      a1, LAB_SENTINEL_A1
    li      a2, 0xa2
    li      a3, 0xa3
    li      a4, 0xa4
    li      a5, 0xa5
    li      a6, LAB_SENTINEL_A6
    li      a7, LAB_SYSCALL_PROBE
    ecall

    li      t0, LAB_SYSCALL_RETURN_MAGIC
    bne     a0, t0, .Luser_failed
    li      t0, LAB_SENTINEL_S0
    bne     s0, t0, .Luser_failed
    li      t0, LAB_SENTINEL_S11
    bne     s11, t0, .Luser_failed
    li      t0, LAB_SENTINEL_GP
    bne     gp, t0, .Luser_failed
    li      t0, LAB_SENTINEL_TP
    bne     tp, t0, .Luser_failed
    li      s1, 0
    j       .Luser_breakpoint
.Luser_failed:
    li      s1, 1

.Luser_breakpoint:
    .word   0x00100073
    li      t0, LAB_SENTINEL_S0
    beq     s0, t0, .Luser_exit
    li      s1, 1
.Luser_exit:
    mv      a0, s1
    li      a7, LAB_SYSCALL_EXIT
    ecall
.Luser_hang:
    j       .Luser_hang
.size lab_user_probe, .-lab_user_probe
.option pop

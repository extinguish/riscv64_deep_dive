#include "lab_constants.h"

.section .text.flow
.align 2
.globl lab_s_kernel_entry
.type lab_s_kernel_entry, @function
lab_s_kernel_entry:
    lla     sp, __s_trap_stack_top
    lla     t0, lab_s_trap_entry
    csrw    stvec, t0
    csrw    sscratch, sp
    csrw    sie, zero

    lla     t0, lab_u_workload
    csrw    sepc, t0
    csrr    t0, sstatus
    li      t1, ~((1 << 8) | (1 << 5) | (1 << 1))
    and     t0, t0, t1
    csrw    sstatus, t0

    lla     sp, __u_stack_top
    li      gp, LAB_USER_GP_SENTINEL
    li      tp, LAB_USER_TP_SENTINEL
    sret
.size lab_s_kernel_entry, .-lab_s_kernel_entry

.align 2
.globl lab_u_workload
.type lab_u_workload, @function
lab_u_workload:
    li      s4, 0x54
    li      s5, 0x55
    li      s6, 0x56
    li      s7, 0x57
    li      s8, 0x58
    li      s9, 0x59
    li      s10, 0x5a
    li      s11, 0x5b

    li      a0, 0x1234
    li      a1, 0x5678
    li      a7, LAB_SYSCALL_ADD
    ecall
    mv      s0, a0

    li      a0, 0xff00
    li      a1, 0x0ff0
    li      a7, LAB_SYSCALL_XOR
    ecall
    mv      s1, a0

    .option push
    .option norvc
    ebreak
    .option pop
    c.ebreak

    li      a5, 0
    .balign 4
    .word   ((15 << 7) | 0x0b)
    mv      s2, a5

    mv      a0, s0
    mv      a1, s1
    mv      a2, s2
    li      a7, LAB_SYSCALL_FINISH
    ecall
1:
    j       1b
.size lab_u_workload, .-lab_u_workload

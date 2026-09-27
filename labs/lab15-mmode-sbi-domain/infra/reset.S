.section .text.init
.align 2
.globl _start
_start:
    csrr a0, mhartid
    bnez a0, .Lpark
    lla sp, __m_stack_top
    .option push
    .option norelax
    lla gp, __global_pointer$
    .option pop
    lla t0, lab_m_trap
    csrw mtvec, t0
    csrw mie, zero
    csrci mstatus, 8
    lla t0, __bss_start
    lla t1, __bss_end
.Lzero:
    bgeu t0, t1, .Ldone
    sd zero, 0(t0)
    addi t0, t0, 8
    j .Lzero
.Ldone:
    tail lab_main
.Lpark:
    wfi
    j .Lpark

.section .text
.align 2
.globl lab_enter_s
lab_enter_s:
    csrw mscratch, sp
    csrw mepc, a0
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x800
    csrs mstatus, t0
    mv sp, a1
    mret

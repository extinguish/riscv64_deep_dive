.section .text.init
.align 2
.globl _start
_start:
    csrr a0, mhartid
    bnez a0, .Lpark
    lla sp, __stack_top
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
    csrr t0, misa
    andi t0, t0, 128
    beqz t0, .Lno_h
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0
    lla t0, lab_main
    csrw mepc, t0
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x800
    csrs mstatus, t0
    li t0, ((1 << 10) | (1 << 20) | (1 << 21) | (1 << 23))
    csrw medeleg, t0
    csrw mideleg, zero
    mret
.Lpark:
    wfi
    j .Lpark
.Lno_h:
    tail lab_no_h
.globl lab_m_trap
lab_m_trap:
    lla t0, lab_m_fatal
    jr t0

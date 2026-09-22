#include "lab_layout.h"

.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    csrci   mstatus, 8
    csrw    mie, zero
    csrr    a0, mhartid
    li      t0, LAB_HART_COUNT
    bgeu    a0, t0, .Lpark

    lla     sp, __emergency_stacks_start
    slli    t0, a0, 9
    add     sp, sp, t0
    addi    sp, sp, LAB_EMERGENCY_STACK_SIZE

    bnez    a0, .Lwait_bss
    lla     t0, __bss_start
    lla     t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    fence   rw, w
    lla     t0, lab_boot_ready
    li      t1, 1
    sd      t1, 0(t0)
    j       .Lruntime
.Lwait_bss:
    lla     t0, lab_boot_ready
3:
    ld      t1, 0(t0)
    beqz    t1, 3b
    fence   r, rw

.Lruntime:
    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop
    lla     sp, __hart_stacks_start
    addi    t0, a0, 1
    slli    t0, t0, 12
    add     sp, sp, t0
    lla     tp, __per_hart_start
    slli    t0, a0, 6
    add     tp, tp, t0
    tail    lab_hart_entry

.Lpark:
    wfi
    j       .Lpark
.size _start, .-_start

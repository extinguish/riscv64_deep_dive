#include "lab_layout.h"

.section .data.bootstrap,"aw"
.align 3
.globl lab_infra_boot_lock
lab_infra_boot_lock:
    .word 0
.globl lab_infra_boot_ready
lab_infra_boot_ready:
    .word 0

.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    csrci   mstatus, 8
    csrw    mie, zero
    csrr    a0, mhartid
    li      t0, LAB_HART_COUNT
    bgeu    a0, t0, .Lunknown_hart
    mv      s0, a0

    lla     sp, __hart_stacks_start
    li      t0, LAB_HART_STACK_SIZE
    mul     t1, s0, t0
    add     sp, sp, t1
    add     sp, sp, t0
    andi    sp, sp, -16

    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop

    lla     t0, lab_trap_entry
    csrw    mtvec, t0

    lla     t0, lab_infra_boot_lock
    li      t1, 1
    amoswap.w.aq t2, t1, (t0)
    bnez    t2, .Lwait_for_bss
    lla     t0, __bss_start
    lla     t1, __bss_end
.Lclear_bss:
    bgeu    t0, t1, .Lpublish_bss
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .Lclear_bss
.Lpublish_bss:
    fence   rw, rw
    lla     t0, lab_infra_boot_ready
    li      t1, 1
    sw      t1, 0(t0)
    j       .Lbss_ready
.Lwait_for_bss:
    lla     t0, lab_infra_boot_ready
.Lwait_for_bss_loop:
    lw      t1, 0(t0)
    fence   r, rw
    beqz    t1, .Lwait_for_bss_loop
.Lbss_ready:
    lla     tp, __per_hart_start
    slli    t0, s0, 6
    add     tp, tp, t0
    mv      a0, s0
    tail    lab_hart_entry

.Lunknown_hart:
    wfi
    j       .Lunknown_hart
.size _start, .-_start

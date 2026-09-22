#include "lab_layout.h"

.section .text
.align 2
.globl lab_boot_entry
.type lab_boot_entry, @function
lab_boot_entry:
    bnez    a0, .Lunit_park

    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop

    lla     sp, __hart_stacks_start
    li      t0, LAB_HART_STACK_SIZE
    add     sp, sp, t0
    andi    sp, sp, -16
    lla     tp, __per_hart_start
    tail    lab_boot_unit_main
.Lunit_park:
    wfi
    j       .Lunit_park
.size lab_boot_entry, .-lab_boot_entry


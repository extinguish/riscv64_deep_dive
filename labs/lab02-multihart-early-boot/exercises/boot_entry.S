#include "lab_layout.h"

.section .text.boot.exercise
.align 2

/*
 * TODO 4: implement the complete multi-hart early-boot protocol documented
 * in README.md. Entry: a0=mhartid, a1=FDT, emergency sp is valid. gp and tp
 * are intentionally not initialized by the reset shim.
 */
.globl lab_boot_entry
.type lab_boot_entry, @function
lab_boot_entry:
    bnez    a0, .Ltodo_park
    lla     a0, .Ltodo_message
    call    lab_early_puts
    li      a0, 1
    tail    lab_early_exit
.Ltodo_park:
    wfi
    j       .Ltodo_park
.size lab_boot_entry, .-lab_boot_entry

.section .rodata
.Ltodo_message:
    .asciz "LAB02 TODO: lab_boot_entry is not implemented\n"

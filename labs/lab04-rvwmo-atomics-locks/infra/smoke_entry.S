#include "lab_layout.h"

.section .text.flow
.align 2
.globl lab_hart_entry
.type lab_hart_entry, @function
lab_hart_entry:
    li      t0, 1
    lla     t1, lab_smoke_arrived
    amoadd.d.aqrl zero, t0, (t1)
    bnez    a0, .Lpark
1:
    ld      t0, 0(t1)
    li      t2, LAB_HART_COUNT
    bne     t0, t2, 1b
    lla     a0, .Lpass
    call    lab_early_puts
    li      a0, 0
    tail    lab_early_exit
.Lpark:
    wfi
    j       .Lpark
.size lab_hart_entry, .-lab_hart_entry

.section .rodata
.Lpass:
    .asciz "LAB04 infrastructure smoke: 4 harts and shared AMO work\nSMOKE PASS\n"

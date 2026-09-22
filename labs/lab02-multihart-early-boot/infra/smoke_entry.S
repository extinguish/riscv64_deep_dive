#include "lab_layout.h"

.section .text
.align 2
.globl lab_boot_entry
.type lab_boot_entry, @function
lab_boot_entry:
    li      t0, 1
    sll     t0, t0, a0
    lla     t1, lab_smoke_seen
    amoor.d.aqrl zero, t0, (t1)

    li      t0, 1
    lla     t1, lab_smoke_arrived
    amoadd.d.aqrl zero, t0, (t1)

    bnez    a0, .Lsmoke_park
1:
    ld      t0, 0(t1)
    li      t2, LAB_HART_COUNT
    bne     t0, t2, 1b
    lla     a0, .Lsmoke_pass
    call    lab_early_puts
    li      a0, 0
    tail    lab_early_exit
.Lsmoke_park:
    wfi
    j       .Lsmoke_park
.size lab_boot_entry, .-lab_boot_entry

.section .rodata
.Lsmoke_pass:
    .asciz "LAB02 infrastructure smoke: 4 harts reached reset entry\nSMOKE PASS\n"


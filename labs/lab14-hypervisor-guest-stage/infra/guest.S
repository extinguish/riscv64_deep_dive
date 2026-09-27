.section .text.guest
.align 2
.globl lab_guest_entry
lab_guest_entry:
    li t0, 0x40000000
    ld a0, 0(t0)
    sd a0, 0(t0)
    li t0, 0x10000000
    sd a0, 0(t0)
    ecall
.Lguest_hang:
    j .Lguest_hang

.section .text
.align 2
.globl lab_enter_guest
lab_enter_guest:
    csrw sepc, a0
    csrw sscratch, sp
    mv sp, a1
    li t0, 0x100
    csrs sstatus, t0
    li t0, 0x80
    csrs hstatus, t0
    sret

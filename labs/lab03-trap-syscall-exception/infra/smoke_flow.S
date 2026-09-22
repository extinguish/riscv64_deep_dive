.section .text.flow
.align 2
.globl lab_s_kernel_entry
.type lab_s_kernel_entry, @function
lab_s_kernel_entry:
    lla     sp, __s_trap_stack_top
    lla     t0, .Lsmoke_user
    csrw    sepc, t0
    csrr    t0, sstatus
    li      t1, ~((1 << 8) | (1 << 5) | (1 << 1))
    and     t0, t0, t1
    csrw    sstatus, t0
    lla     sp, __u_stack_top
    sret
.size lab_s_kernel_entry, .-lab_s_kernel_entry

.Lsmoke_user:
    lla     a0, .Lsmoke_message
    call    lab_early_puts
    li      a0, 0
    tail    lab_early_exit

.section .rodata
.Lsmoke_message:
    .asciz "LAB03 infrastructure smoke: M -> S -> U transition works\nSMOKE PASS\n"

.section .bss
.align 3
.globl lab_fault_count
lab_fault_count:
    .dword 0
.globl lab_fault_cause
lab_fault_cause:
    .dword 0
.globl lab_fault_value
lab_fault_value:
    .dword 0

.section .text
.align 2
.globl lab_s_trap_vector
.type lab_s_trap_vector, @function
lab_s_trap_vector:
    csrr    t0, scause
    lla     t1, lab_fault_cause
    sd      t0, (t1)
    csrr    t0, stval
    lla     t1, lab_fault_value
    sd      t0, (t1)
    lla     t1, lab_fault_count
    ld      t0, (t1)
    addi    t0, t0, 1
    sd      t0, (t1)

    csrr    t0, sepc
    lla     t1, lab_try_load64_fault
    beq     t0, t1, .Lload_recover
    lla     t1, lab_try_store64_fault
    beq     t0, t1, .Lstore_recover

    li      a0, 1
    tail    lab_exit

.Lload_recover:
    lla     t0, lab_try_load64_recover
    csrw    sepc, t0
    sret
.Lstore_recover:
    lla     t0, lab_try_store64_recover
    csrw    sepc, t0
    sret
.size lab_s_trap_vector, .-lab_s_trap_vector

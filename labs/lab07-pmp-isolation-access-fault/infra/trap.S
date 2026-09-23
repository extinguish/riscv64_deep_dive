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
.globl lab_m_trap_vector
.type lab_m_trap_vector, @function
lab_m_trap_vector:
    csrr    t0, mcause
    lla     t1, lab_fault_cause
    sd      t0, (t1)
    csrr    t0, mtval
    lla     t1, lab_fault_value
    sd      t0, (t1)
    lla     t1, lab_fault_count
    ld      t0, (t1)
    addi    t0, t0, 1
    sd      t0, (t1)

    csrr    t0, mepc
    lla     t1, lab_mprv_load_fault
    beq     t0, t1, .Lload_recover
    lla     t1, lab_mprv_store_fault
    beq     t0, t1, .Lstore_recover
    li      a0, 1
    tail    lab_exit

.Lload_recover:
    lla     t0, lab_mprv_load_recover
    csrw    mepc, t0
    mret
.Lstore_recover:
    lla     t0, lab_mprv_store_recover
    csrw    mepc, t0
    mret
.size lab_m_trap_vector, .-lab_m_trap_vector

.section .text.flow
.align 2
.globl lab_m_trap_vector
.type lab_m_trap_vector, @function
lab_m_trap_vector:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    lla     sp, __emergency_stack_top
    andi    sp, sp, -16
    tail    lab_m_trap_fail
.size lab_m_trap_vector, .-lab_m_trap_vector

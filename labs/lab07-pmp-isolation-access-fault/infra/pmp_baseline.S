.section .test_page, "aw", @nobits
.align 12
.globl lab_pmp_test_page
lab_pmp_test_page:
    .space 4096

.section .text
.align 2
.globl lab_pmp_baseline_init
.type lab_pmp_baseline_init, @function
lab_pmp_baseline_init:
    csrw    pmpcfg0, zero
    csrw    pmpaddr0, zero
    li      t0, -1
    csrw    pmpaddr1, t0
    li      t0, 0x1f00
    csrw    pmpcfg0, t0
    lla     t0, lab_m_trap_vector
    csrw    mtvec, t0
    fence   rw, rw
    ret
.size lab_pmp_baseline_init, .-lab_pmp_baseline_init

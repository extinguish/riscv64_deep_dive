#include "lab_layout.h"
.section .text.exercise
.align 2
.globl lab_domain_range
lab_domain_range:
    # TODO: return 1 only for an aligned nonempty 8-byte-granular range
    # wholly inside [base,end), with no arithmetic overflow.
    li a0, 0
    ret

.globl lab_pmp_napot
lab_pmp_napot:
    # TODO: encode an aligned power-of-two NAPOT interval of at least 8B.
    # Reject invalid sizes, alignment and base+size overflow with UINT64_MAX.
    li a0, -1
    ret

.globl lab_sbi_dispatch
lab_sbi_dispatch:
    # TODO: only modify frame a0/a1. Implement SBI Base FID 0..6 and
    # lab experimental EID FID 0/1. FID 0 validates an S-owned 8B buffer
    # through lab_domain_range before loading it. All other EID/FID pairs
    # return SBI_ERR_NOT_SUPPORTED. See README for exact table.
    li t0, -2
    sd t0, F_A0(a0)
    sd zero, F_A1(a0)
    ret

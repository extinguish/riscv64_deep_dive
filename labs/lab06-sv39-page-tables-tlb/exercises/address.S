#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1a: int lab_sv39_is_canonical(uint64_t va) */
.globl lab_sv39_is_canonical
.type lab_sv39_is_canonical, @function
lab_sv39_is_canonical:
    li      a0, 0
    ret
.size lab_sv39_is_canonical, .-lab_sv39_is_canonical

/* TODO 1b: uint64_t lab_sv39_vpn(uint64_t va, uint64_t level) */
.globl lab_sv39_vpn
.type lab_sv39_vpn, @function
lab_sv39_vpn:
    li      a0, -1
    ret
.size lab_sv39_vpn, .-lab_sv39_vpn

/* TODO 1c: uint64_t lab_make_satp(uint64_t *root, uint64_t asid) */
.globl lab_make_satp
.type lab_make_satp, @function
lab_make_satp:
    li      a0, 0
    ret
.size lab_make_satp, .-lab_make_satp

/* TODO 1d: int lab_switch_address_space(uint64_t *root, uint64_t asid) */
.globl lab_switch_address_space
.type lab_switch_address_space, @function
lab_switch_address_space:
    li      a0, 0
    ret
.size lab_switch_address_space, .-lab_switch_address_space

#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 5: encode an APLIC direct-delivery target register. */
.globl lab_aplic_direct_target
.type lab_aplic_direct_target, @function
lab_aplic_direct_target:
    li      a0, -1
    ret
.size lab_aplic_direct_target, .-lab_aplic_direct_target

/* TODO 6: encode an APLIC MSI target with reserved bit 11 clear. */
.globl lab_aplic_msi_target
.type lab_aplic_msi_target, @function
lab_aplic_msi_target:
    li      a0, -1
    ret
.size lab_aplic_msi_target, .-lab_aplic_msi_target

/* TODO 7: atomically allocate an IMSIC guest file, excluding index zero. */
.globl lab_imsic_guest_allocate
.type lab_imsic_guest_allocate, @function
lab_imsic_guest_allocate:
    li      a0, -1
    ret
.size lab_imsic_guest_allocate, .-lab_imsic_guest_allocate

/* TODO 8: serialize an xiselect/xireg-style window and restore selector. */
.globl lab_indirect_window_access
.type lab_indirect_window_access, @function
lab_indirect_window_access:
    li      a0, -1
    ret
.size lab_indirect_window_access, .-lab_indirect_window_access

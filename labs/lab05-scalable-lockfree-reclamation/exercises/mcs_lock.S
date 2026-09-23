#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 1a: void lab_mcs_lock(lock, node) */
.globl lab_mcs_lock
.type lab_mcs_lock, @function
lab_mcs_lock:
    ret
.size lab_mcs_lock, .-lab_mcs_lock

/* TODO 1b: void lab_mcs_unlock(lock, node) */
.globl lab_mcs_unlock
.type lab_mcs_unlock, @function
lab_mcs_unlock:
    ret
.size lab_mcs_unlock, .-lab_mcs_unlock

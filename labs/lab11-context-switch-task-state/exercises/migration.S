#include "lab_layout.h"
.section .text.exercise
.align 2
/* TODO 5: validate target-hart capabilities and lazy-owner handoff. */
.globl lab_migration_allowed
.type lab_migration_allowed, @function
lab_migration_allowed:
    li a0, 0
    ret
.size lab_migration_allowed, .-lab_migration_allowed

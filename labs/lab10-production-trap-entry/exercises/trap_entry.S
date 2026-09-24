#include "lab_layout.h"

.section .text.exercise
.align 2

/*
 * TODO 3: replace this fail-fast stub with a complete S-mode trap entry and
 * exit path using struct lab_pt_regs. Keep the stub until save/restore is
 * complete so an unfinished exercise fails rather than corrupting state.
 */
.globl lab_s_trap_vector
.type lab_s_trap_vector, @function
lab_s_trap_vector:
    tail    lab_trap_unimplemented
.size lab_s_trap_vector, .-lab_s_trap_vector

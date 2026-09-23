#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4a: void lab_stack_push(stack, node) */
.globl lab_stack_push
.type lab_stack_push, @function
lab_stack_push:
    ret
.size lab_stack_push, .-lab_stack_push

/* TODO 4b: node *lab_stack_pop_protected(stack, hazard_slot) */
.globl lab_stack_pop_protected
.type lab_stack_pop_protected, @function
lab_stack_pop_protected:
    li      a0, 0
    ret
.size lab_stack_pop_protected, .-lab_stack_pop_protected

/* TODO 4c: void lab_hazard_clear(hazard_slot) */
.globl lab_hazard_clear
.type lab_hazard_clear, @function
lab_hazard_clear:
    ret
.size lab_hazard_clear, .-lab_hazard_clear

/* TODO 4d: int lab_hazard_is_protected(hazards, count, candidate) */
.globl lab_hazard_is_protected
.type lab_hazard_is_protected, @function
lab_hazard_is_protected:
    li      a0, 0
    ret
.size lab_hazard_is_protected, .-lab_hazard_is_protected

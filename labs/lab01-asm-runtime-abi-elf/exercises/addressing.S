.section .text
.align 2

/* TODO 5a: return the address of lab_global_anchor using PC-relative code. */
.globl asm_global_address
.type asm_global_address, @function
asm_global_address:
    li      a0, 0
    ret
.size asm_global_address, .-asm_global_address

/* TODO 5b: load and return the uint64_t stored at lab_global_anchor. */
.globl asm_global_load
.type asm_global_load, @function
asm_global_load:
    li      a0, 0
    ret
.size asm_global_load, .-asm_global_load

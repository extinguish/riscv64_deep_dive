.section .text
.align 2

/* TODO 1: void asm_memzero(void *start, size_t size) */
.globl asm_memzero
.type asm_memzero, @function
asm_memzero:
    ret
.size asm_memzero, .-asm_memzero

/* TODO 2: void *asm_memmove(void *dst, const void *src, size_t size) */
.globl asm_memmove
.type asm_memmove, @function
asm_memmove:
    ret
.size asm_memmove, .-asm_memmove

/* TODO 3: void asm_add128(out[2], lhs[2], rhs[2]) */
.globl asm_add128
.type asm_add128, @function
asm_add128:
    ret
.size asm_add128, .-asm_add128

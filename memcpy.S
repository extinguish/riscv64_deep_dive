/*
 * void *asm_memcpy(void *dst, const void *src, unsigned long count)
 *
 * RISC-V calling convention: a0=dst, a1=src, a2=count; return value=a0.
 * This has memcpy semantics: source and destination must not overlap.
 */
.section .text
.globl asm_memcpy
.type asm_memcpy, @function
asm_memcpy:
    mv t0, a0                    /* preserve original dst for return */
    beqz a2, .Ldone

.Lcopy_byte:
    lbu t1, 0(a1)                /* load one unsigned byte */
    sb t1, 0(a0)                 /* store it */
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lcopy_byte

.Ldone:
    mv a0, t0
    ret
.size asm_memcpy, .-asm_memcpy


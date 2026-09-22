.section .lab_blob, "ax", @progbits
.align 2

/*
 * TODO 6: return a0 * 3 + 7.
 *
 * The complete implementation must remain inside this section and must not
 * refer to any symbol outside __lab_blob_start..__lab_blob_end.
 */
.globl asm_pic_blob
.type asm_pic_blob, @function
asm_pic_blob:
    li      a0, -1
    ret
.size asm_pic_blob, .-asm_pic_blob

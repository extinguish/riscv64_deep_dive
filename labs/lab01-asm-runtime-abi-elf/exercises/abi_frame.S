.section .text
.align 2

/*
 * TODO 4: uint64_t asm_call_checksum(const uint64_t *values, size_t count)
 *
 * Call lab_checksum_step() once per element. Add the .cfi_* directives that
 * describe the stack frame you create; do not copy the infrastructure frame
 * blindly because its layout is deliberately different.
 */
.globl asm_call_checksum
.type asm_call_checksum, @function
asm_call_checksum:
    .cfi_startproc
    li      a0, -1
    ret
    .cfi_endproc
.size asm_call_checksum, .-asm_call_checksum

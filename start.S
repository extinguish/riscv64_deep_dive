/* Minimal RV64 startup code for QEMU's virt machine. */
.section .text.init
.globl _start
_start:
    la sp, __stack_top

    /* Clear .bss before calling C. */
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    call main

    /* main returns 0 on success, non-zero on failure. */
    beqz a0, 3f
    li t0, 0x3333                 /* QEMU sifive_test: failure */
    j 4f
3:
    li t0, 0x5555                 /* QEMU sifive_test: success */
4:
    li t1, 0x100000
    sw t0, 0(t1)
5:
    wfi
    j 5b


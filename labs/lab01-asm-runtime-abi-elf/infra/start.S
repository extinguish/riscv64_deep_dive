.section .text.init
.align 2
.globl _start
.type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, __stack_top
    la      t0, lab_fatal_vector
    csrw    mtvec, t0

    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    call    main

    beqz    a0, 3f
    /* SiFive finisher: high bits carry the non-zero host exit status. */
    li      t0, 0x13333
    j       4f
3:
    li      t0, 0x5555
4:
    li      t1, 0x100000
    sw      t0, 0(t1)
5:
    wfi
    j       5b
.size _start, .-_start

.align 2
.type lab_fatal_vector, @function
lab_fatal_vector:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    tail    lab_fatal_trap
.size lab_fatal_vector, .-lab_fatal_vector

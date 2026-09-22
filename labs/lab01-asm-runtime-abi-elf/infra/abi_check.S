.section .text
.align 2
.globl lab_check_callee_saved
.type lab_check_callee_saved, @function
lab_check_callee_saved:
    .cfi_startproc
    addi    sp, sp, -144
    .cfi_def_cfa_offset 144
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)
    .cfi_offset ra, -144
    .cfi_offset s0, -136
    .cfi_offset s1, -128
    .cfi_offset s2, -120
    .cfi_offset s3, -112
    .cfi_offset s4, -104
    .cfi_offset s5, -96
    .cfi_offset s6, -88
    .cfi_offset s7, -80
    .cfi_offset s8, -72
    .cfi_offset s9, -64
    .cfi_offset s10, -56
    .cfi_offset s11, -48

    sd      a0, 104(sp)
    sd      a1, 112(sp)
    sd      a2, 120(sp)
    sd      a3, 128(sp)

    li      s0, 0x10
    li      s1, 0x11
    li      s2, 0x12
    li      s3, 0x13
    li      s4, 0x14
    li      s5, 0x15
    li      s6, 0x16
    li      s7, 0x17
    li      s8, 0x18
    li      s9, 0x19
    li      s10, 0x1a
    li      s11, 0x1b

    ld      a0, 112(sp)
    ld      a1, 120(sp)
    ld      t0, 104(sp)
    jalr    ra, t0, 0
    ld      t0, 128(sp)
    sd      a0, 0(t0)

    li      t1, 0
    li      t0, 0x10
    beq     s0, t0, 1f
    ori     t1, t1, 0x001
1:  li      t0, 0x11
    beq     s1, t0, 2f
    ori     t1, t1, 0x002
2:  li      t0, 0x12
    beq     s2, t0, 3f
    ori     t1, t1, 0x004
3:  li      t0, 0x13
    beq     s3, t0, 4f
    ori     t1, t1, 0x008
4:  li      t0, 0x14
    beq     s4, t0, 5f
    ori     t1, t1, 0x010
5:  li      t0, 0x15
    beq     s5, t0, 6f
    ori     t1, t1, 0x020
6:  li      t0, 0x16
    beq     s6, t0, 7f
    ori     t1, t1, 0x040
7:  li      t0, 0x17
    beq     s7, t0, 8f
    ori     t1, t1, 0x080
8:  li      t0, 0x18
    beq     s8, t0, 9f
    ori     t1, t1, 0x100
9:  li      t0, 0x19
    beq     s9, t0, 10f
    ori     t1, t1, 0x200
10: li      t0, 0x1a
    beq     s10, t0, 11f
    ori     t1, t1, 0x400
11: li      t0, 0x1b
    beq     s11, t0, 12f
    li      t0, 0x800
    or      t1, t1, t0
12:
    mv      a0, t1
    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, 144
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
.size lab_check_callee_saved, .-lab_check_callee_saved

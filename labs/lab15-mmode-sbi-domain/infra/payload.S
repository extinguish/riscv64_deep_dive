.section .payload_data,"aw"
.align 3
.globl lab_s_word
lab_s_word:
    .dword 0x59beef1234cafe67

.section .payload_text,"ax"
.align 2
.globl lab_s_smoke
lab_s_smoke:
    li a7, 0x08000000
    li a6, 1
    li a0, 1
    ecall
.Lsmoke_hang:
    j .Lsmoke_hang

.align 2
.globl lab_s_payload
lab_s_payload:
    li s0, 0x123456789abcdef0
    li a2, 0x2468
    li a7, 0x10
    li a6, 0
    ecall
    bnez a0, .Lfail
    li t0, 0x03000000
    bne a1, t0, .Lfail
    li t0, 0x123456789abcdef0
    bne s0, t0, .Lfail
    li t0, 0x2468
    bne a2, t0, .Lfail
    li t0, 0x10
    bne a7, t0, .Lfail

    li a7, 0x10
    li a6, 1
    ecall
    bnez a0, .Lfail
    li t0, 0x4c4142
    bne a1, t0, .Lfail

    li a7, 0x10
    li a6, 3
    li a0, 0x08000000
    ecall
    bnez a0, .Lfail
    li t0, 1
    bne a1, t0, .Lfail

    li a7, 0x08000000
    li a6, 0
    lla a0, lab_s_word
    ecall
    bnez a0, .Lfail
    li t0, 0x59beef1234cafe67
    bne a1, t0, .Lfail

    li a7, 0x08000000
    li a6, 0
    lla a0, lab_firmware_secret
    ecall
    li t0, -5
    bne a0, t0, .Lfail
    bnez a1, .Lfail

    lla t0, lab_firmware_secret
    .option push
    .option norvc
    sd zero, 0(t0)
    .option pop

    li a7, 0x08000000
    li a6, 1
    li a0, 1
    ecall
    j .Lsmoke_hang
.Lfail:
    li a7, 0x08000000
    li a6, 1
    li a0, 0
    ecall
    j .Lsmoke_hang

#ifndef LAB_LAYOUT_H
#define LAB_LAYOUT_H

#define LAB_STACK_SIZE 16384
#define LAB_EMERGENCY_STACK_SIZE 4096

#define LAB_X_ZERO 0
#define LAB_X_RA 1
#define LAB_X_SP 2
#define LAB_X_GP 3
#define LAB_X_TP 4
#define LAB_X_T0 5
#define LAB_X_T1 6
#define LAB_X_T2 7
#define LAB_X_S0 8
#define LAB_X_S1 9
#define LAB_X_A0 10
#define LAB_X_A1 11
#define LAB_X_A2 12
#define LAB_X_A3 13
#define LAB_X_A4 14
#define LAB_X_A5 15
#define LAB_X_A6 16
#define LAB_X_A7 17
#define LAB_X_S2 18
#define LAB_X_S3 19
#define LAB_X_S4 20
#define LAB_X_S5 21
#define LAB_X_S6 22
#define LAB_X_S7 23
#define LAB_X_S8 24
#define LAB_X_S9 25
#define LAB_X_S10 26
#define LAB_X_S11 27
#define LAB_X_T3 28
#define LAB_X_T4 29
#define LAB_X_T5 30
#define LAB_X_T6 31

#define LAB_PT_ZERO 0
#define LAB_PT_RA 8
#define LAB_PT_SP 16
#define LAB_PT_GP 24
#define LAB_PT_TP 32
#define LAB_PT_T0 40
#define LAB_PT_T1 48
#define LAB_PT_T2 56
#define LAB_PT_S0 64
#define LAB_PT_S1 72
#define LAB_PT_A0 80
#define LAB_PT_A1 88
#define LAB_PT_A2 96
#define LAB_PT_A3 104
#define LAB_PT_A4 112
#define LAB_PT_A5 120
#define LAB_PT_A6 128
#define LAB_PT_A7 136
#define LAB_PT_S2 144
#define LAB_PT_S3 152
#define LAB_PT_S4 160
#define LAB_PT_S5 168
#define LAB_PT_S6 176
#define LAB_PT_S7 184
#define LAB_PT_S8 192
#define LAB_PT_S9 200
#define LAB_PT_S10 208
#define LAB_PT_S11 216
#define LAB_PT_T3 224
#define LAB_PT_T4 232
#define LAB_PT_T5 240
#define LAB_PT_T6 248
#define LAB_PT_SSTATUS 256
#define LAB_PT_SEPC 264
#define LAB_PT_SCAUSE 272
#define LAB_PT_STVAL 280
#define LAB_PT_ORIG_A0 288
#define LAB_PT_FLAGS 296
#define LAB_PT_HARTID 304
#define LAB_PT_RESERVED 312
#define LAB_PT_SIZE 320

#define LAB_FRAME_FROM_USER 1

#define LAB_SSTATUS_SIE 0x2
#define LAB_SSTATUS_SPIE 0x20
#define LAB_SSTATUS_SPP 0x100
#define LAB_SSTATUS_VS_MASK 0x600
#define LAB_SSTATUS_FS_MASK 0x6000
#define LAB_SSTATUS_SUM 0x40000
#define LAB_SSTATUS_MXR 0x80000

#define LAB_PRIV_U 0
#define LAB_PRIV_S 1
#define LAB_SCAUSE_INTERRUPT_BIT 0x8000000000000000
#define LAB_SCAUSE_CODE_MASK 0x7fffffffffffffff
#define LAB_CAUSE_BREAKPOINT 3
#define LAB_CAUSE_USER_ECALL 8

#define LAB_SYSCALL_PROBE 1
#define LAB_SYSCALL_EXIT 2
#define LAB_SYSCALL_RETURN_MAGIC 0x5a5aa5a5

#define LAB_SENTINEL_RA 0x101
#define LAB_SENTINEL_GP 0x303
#define LAB_SENTINEL_TP 0x404
#define LAB_SENTINEL_T0 0x500
#define LAB_SENTINEL_T1 0x510
#define LAB_SENTINEL_T2 0x520
#define LAB_SENTINEL_S0 0x800
#define LAB_SENTINEL_S1 0x810
#define LAB_SENTINEL_S11 0x8b0
#define LAB_SENTINEL_A0 0xa0
#define LAB_SENTINEL_A1 0xa1
#define LAB_SENTINEL_A6 0xa6

#endif

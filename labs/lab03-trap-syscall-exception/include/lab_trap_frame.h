#ifndef LAB_TRAP_FRAME_H
#define LAB_TRAP_FRAME_H

#define TF_X0       0
#define TF_RA       8
#define TF_SP       16
#define TF_GP       24
#define TF_TP       32
#define TF_T0       40
#define TF_T1       48
#define TF_T2       56
#define TF_S0       64
#define TF_S1       72
#define TF_A0       80
#define TF_A1       88
#define TF_A2       96
#define TF_A3       104
#define TF_A4       112
#define TF_A5       120
#define TF_A6       128
#define TF_A7       136
#define TF_S2       144
#define TF_S3       152
#define TF_S4       160
#define TF_S5       168
#define TF_S6       176
#define TF_S7       184
#define TF_S8       192
#define TF_S9       200
#define TF_S10      208
#define TF_S11      216
#define TF_T3       224
#define TF_T4       232
#define TF_T5       240
#define TF_T6       248
#define TF_SEPC     256
#define TF_SSTATUS  264
#define TF_SCAUSE   272
#define TF_STVAL    280
#define TF_SIZE     288

#ifndef __ASSEMBLER__
#include <stdint.h>

struct lab_trap_frame {
    uint64_t x[32];
    uint64_t sepc;
    uint64_t sstatus;
    uint64_t scause;
    uint64_t stval;
};
#endif

#endif

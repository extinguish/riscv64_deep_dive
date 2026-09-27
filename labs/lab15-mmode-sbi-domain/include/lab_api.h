#ifndef LAB_API_H
#define LAB_API_H
#include <stdint.h>
#include "lab_layout.h"

#define LAB_INVALID UINT64_MAX
#define LAB_SBI_BASE UINT64_C(0x10)
#define LAB_SBI_EXPERIMENTAL UINT64_C(0x08000000)
#define LAB_SBI_SPEC_V3 UINT64_C(0x03000000)
#define LAB_SBI_IMPL_ID UINT64_C(0x4c4142)
#define LAB_SBI_IMPL_VERSION UINT64_C(1)
#define LAB_SBI_SUCCESS INT64_C(0)
#define LAB_SBI_NOT_SUPPORTED INT64_C(-2)
#define LAB_SBI_INVALID_ADDRESS INT64_C(-5)
#define LAB_S_BASE UINT64_C(0x80200000)
#define LAB_S_END UINT64_C(0x80400000)
#define LAB_S_MAGIC UINT64_C(0x59beef1234cafe67)

typedef struct {
    uint64_t ra,gp,tp,t0,t1,t2,s0,s1,a0,a1,a2,a3,a4,a5,a6,a7;
    uint64_t s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,t3,t4,t5,t6;
    uint64_t s_sp,mcause,mepc,mtval;
} lab_frame;

extern char __m_stack_top[], __s_stack_top[];
extern uint64_t lab_firmware_secret, lab_s_word;
extern void lab_s_payload(void), lab_s_smoke(void), lab_m_trap(void);
extern void lab_enter_s(uint64_t entry, uint64_t stack);
extern int lab_domain_range(uint64_t addr, uint64_t size,
                            uint64_t base, uint64_t end);
extern uint64_t lab_pmp_napot(uint64_t base, uint64_t size);
extern void lab_sbi_dispatch(lab_frame *frame);
void lab_puts(const char *s);
void lab_puthex(uint64_t value);
unsigned lab_report(const char *name, int ok);
__attribute__((noreturn)) void lab_exit(unsigned status);
void lab_main(void);
void lab_trap_dispatch(lab_frame *frame);
#endif

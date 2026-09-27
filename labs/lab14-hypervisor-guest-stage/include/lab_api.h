#ifndef LAB_API_H
#define LAB_API_H
#include <stdint.h>
#include "lab_layout.h"

#define LAB_INVALID UINT64_MAX
#define LAB_PTE_V 1u
#define LAB_PTE_R 2u
#define LAB_PTE_W 4u
#define LAB_PTE_X 8u
#define LAB_PTE_U 16u
#define LAB_PTE_A 64u
#define LAB_PTE_D 128u
#define LAB_GUEST_ALIAS UINT64_C(0x40000000)
#define LAB_GUEST_MMIO UINT64_C(0x10000000)
#define LAB_GUEST_MAGIC UINT64_C(0x1234abcddcba4321)
#define LAB_HSTATUS_SPV (UINT64_C(1) << 7)
#define LAB_CAUSE_GUEST_LOAD_FAULT 21u
#define LAB_CAUSE_VS_ECALL 10u

typedef struct {
    uint64_t ra,gp,tp,t0,t1,t2,s0,s1,a0,a1,a2,a3,a4,a5,a6,a7;
    uint64_t s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,t3,t4,t5,t6;
    uint64_t guest_sp,scause,sepc,hstatus;
} lab_frame;

extern char __stack_top[], __guest_stack_top[], __guest_page[];
extern uint64_t lab_gstage_root[2048], lab_alias_l1[512];
extern void lab_guest_entry(void);
extern void lab_hs_trap(void);
extern void lab_main(void);
extern void lab_enter_guest(uint64_t entry, uint64_t stack);
extern uint64_t lab_gstage_index(uint64_t gpa);
extern uint64_t lab_gstage_leaf(uint64_t pa, uint64_t flags);
extern uint64_t lab_guest_fault(uint64_t scause, uint64_t hstatus,
                                uint64_t htval, uint64_t stval);
extern uint64_t lab_install_mapping(uint64_t *slot, uint64_t pte,
                                     uint64_t gpa);
void lab_puts(const char *s);
void lab_puthex(uint64_t value);
unsigned lab_report(const char *name, int ok);
__attribute__((noreturn)) void lab_exit(unsigned status);
__attribute__((noreturn)) void lab_trap_dispatch(lab_frame *frame);
#endif

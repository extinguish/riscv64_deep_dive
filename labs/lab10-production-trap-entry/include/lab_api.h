#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

struct lab_pt_regs {
    uint64_t x[32];
    uint64_t sstatus;
    uint64_t sepc;
    uint64_t scause;
    uint64_t stval;
    uint64_t orig_a0;
    uint64_t flags;
    uint64_t hartid;
    uint64_t reserved;
};

struct lab_cause_decode {
    uint64_t is_interrupt;
    uint64_t cause;
};

uint64_t lab_status_on_trap(uint64_t status, uint64_t previous_privilege);
uint64_t lab_status_on_sret(uint64_t status);
uint64_t lab_sanitize_user_sstatus(uint64_t status);
int lab_decode_scause(uint64_t scause, uint64_t exception_count,
                      uint64_t interrupt_count,
                      struct lab_cause_decode *decoded);
uint64_t lab_advance_epc(uint64_t epc, uint64_t first_halfword);
void lab_s_trap_vector(void);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
unsigned int lab_run_unit_tests(void);
void lab_enter_user(const void *entry, uintptr_t user_sp)
    __attribute__((noreturn));
void lab_user_probe(void) __attribute__((noreturn));
void lab_s_trap_dispatch(struct lab_pt_regs *regs);
void lab_trap_unimplemented(void) __attribute__((noreturn));
void lab_m_trap_fail(uint64_t cause, uint64_t epc, uint64_t tval)
    __attribute__((noreturn));

void *lab_memzero(void *destination, uint64_t size);
void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern volatile uint64_t lab_full_failures;
extern unsigned char lab_vector_example[];
extern unsigned char lab_vector_example_end[];

extern unsigned char __global_pointer$[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];
extern unsigned char __stack_top[];
extern unsigned char __trap_stack_bottom[];
extern unsigned char __trap_stack_top[];
extern unsigned char __user_stack_bottom[];
extern unsigned char __user_stack_top[];
extern unsigned char __emergency_stack_bottom[];
extern unsigned char __emergency_stack_top[];

#endif

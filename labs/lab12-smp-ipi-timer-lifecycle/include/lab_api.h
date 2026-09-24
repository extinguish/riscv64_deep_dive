#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>
#include "lab_layout.h"

uint64_t lab_hartid_to_cpu(uint64_t hartid, const uint64_t *hartids,
                           uint64_t count);
uint64_t lab_hart_mask_chunk(const uint64_t *hartids, uint64_t count,
                             uint64_t hart_mask_base, uint64_t width);
uint64_t lab_cpu_transition(uint64_t state, uint64_t event);
uint64_t lab_ipi_publish(volatile uint64_t *mailbox, uint64_t reasons);
uint64_t lab_ipi_consume(volatile uint64_t *mailbox);
uint64_t lab_wait_for_bits(const volatile uint64_t *word, uint64_t mask);
uint64_t lab_timer_select(uint64_t now, uint64_t local_deadline,
                          uint64_t remote_deadline);
int lab_timer_program(volatile uint64_t *compare, uint64_t deadline);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
void lab_smoke_main(uint64_t hartid, uintptr_t stack_pointer,
                    uintptr_t thread_pointer) __attribute__((noreturn));
void lab_unit_main(uint64_t hartid) __attribute__((noreturn));
void lab_full_main(uint64_t hartid) __attribute__((noreturn));
void lab_trap_entry(void);
void lab_trap_dispatch(uint64_t hartid, uint64_t mcause);
unsigned int lab_run_unit_tests(void);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);
uint64_t lab_read_mtime(void);
volatile uint32_t *lab_msip(uint64_t hartid);
volatile uint64_t *lab_mtimecmp(uint64_t hartid);

extern const uint64_t lab_platform_hartids[LAB_HART_COUNT];
extern unsigned char __global_pointer$[], __bss_start[], __bss_end[];
extern unsigned char __per_hart_start[], __per_hart_end[];
extern unsigned char __hart_stacks_start[], __hart_stacks_end[];

#endif

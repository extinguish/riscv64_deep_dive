#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

uint64_t lab_pmp_napot_encode(uint64_t base, uint64_t size);
int lab_pmp_napot_decode(uint64_t pmpaddr, uint64_t *base_out,
                         uint64_t *size_out);
uint64_t lab_pmp_tor_encode(uint64_t top);
uint64_t lab_pmp_make_cfg(uint64_t permissions, uint64_t mode,
                          uint64_t locked);
int lab_pmp_match(const uint64_t *addresses, uint64_t packed_config,
                  uint64_t count, uint64_t address, uint64_t size,
                  uint64_t access);
int lab_pmp_program_window(uint64_t base, uint64_t size,
                           uint64_t permissions);
int lab_pmp_disable_window(void);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
void lab_pmp_baseline_init(void);
int lab_try_mprv_load64(uint64_t address, uint64_t *output);
int lab_try_mprv_store64(uint64_t address, uint64_t value);
unsigned int lab_run_unit_tests(void);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern uint8_t lab_pmp_test_page[LAB_PAGE_SIZE];
extern volatile uint64_t lab_fault_count;
extern volatile uint64_t lab_fault_cause;
extern volatile uint64_t lab_fault_value;

extern unsigned char __global_pointer$[];
extern unsigned char __stack_top[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

#endif

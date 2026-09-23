#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

int lab_sv39_is_canonical(uint64_t va);
uint64_t lab_sv39_vpn(uint64_t va, uint64_t level);
uint64_t lab_make_satp(uint64_t *root, uint64_t asid);
int lab_switch_address_space(uint64_t *root, uint64_t asid);

uint64_t lab_make_table_pte(uint64_t pa);
uint64_t lab_make_leaf_pte(uint64_t pa, uint64_t flags);

int lab_sv39_translate(uint64_t *root, uint64_t va, uint64_t access,
                       uint64_t *output_pa);
int lab_install_4k(uint64_t *root, uint64_t va, uint64_t pa,
                   uint64_t flags);
int lab_unmap_4k(uint64_t *root, uint64_t va);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
void lab_enter_supervisor(uint64_t entry, uint64_t satp,
                          uint64_t trap_vector) __attribute__((noreturn));
void lab_s_trap_vector(void);
int lab_try_load64(uint64_t address, uint64_t *output);
int lab_try_store64(uint64_t address, uint64_t value);

void lab_prepare_page_tables(int install_smoke_leaf);
uint64_t lab_infra_table_pte(uint64_t pa);
uint64_t lab_infra_leaf_pte(uint64_t pa, uint64_t flags);
uint64_t lab_infra_satp(uint64_t *root, uint64_t asid);
unsigned int lab_run_unit_tests(void);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern uint64_t lab_mmu_root[LAB_PT_ENTRIES];
extern uint64_t lab_mmu_l1[LAB_PT_ENTRIES];
extern uint64_t lab_mmu_l0[LAB_PT_ENTRIES];
extern uint64_t lab_walk_root[LAB_PT_ENTRIES];
extern uint64_t lab_walk_l1[LAB_PT_ENTRIES];
extern uint64_t lab_walk_l0[LAB_PT_ENTRIES];
extern uint8_t lab_test_page_a[LAB_PAGE_SIZE];
extern uint8_t lab_test_page_b[LAB_PAGE_SIZE];

extern volatile uint64_t lab_fault_count;
extern volatile uint64_t lab_fault_cause;
extern volatile uint64_t lab_fault_value;

extern unsigned char __global_pointer$[];
extern unsigned char __stack_top[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

#endif

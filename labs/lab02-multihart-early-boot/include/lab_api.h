#ifndef LAB_API_H
#define LAB_API_H

#include <stddef.h>
#include <stdint.h>

#include "lab_layout.h"

long lab_hartid_to_index(uint64_t hartid, const uint64_t *table, size_t count);
void lab_init_regions(void *data_destination, const void *data_source,
                      size_t data_size, void *bss_start, size_t bss_size);
uint64_t lab_relocated_stage(uint64_t value);

void lab_boot_hart_main(uint64_t hartid, uintptr_t fdt,
                        uint64_t logical_index, uint64_t is_owner)
    __attribute__((noreturn));
void lab_boot_hart_main_c(uint64_t hartid, uintptr_t fdt,
                          uint64_t logical_index, uint64_t is_owner,
                          uintptr_t stack_pointer, uintptr_t global_pointer,
                          uintptr_t thread_pointer) __attribute__((noreturn));

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);
unsigned int lab_run_helper_tests(void);

extern const uint64_t lab_platform_hart_ids[LAB_HART_COUNT];
extern volatile uint64_t lab_boot_owner;
extern volatile uint64_t lab_boot_ready;
extern volatile uint64_t lab_boot_publication;

extern unsigned char __global_pointer$[];
extern unsigned char __hart_stacks_start[];
extern unsigned char __hart_stacks_end[];
extern unsigned char __per_hart_start[];
extern unsigned char __per_hart_end[];
extern unsigned char __lab_data_load_start[];
extern unsigned char __lab_data_load_end[];
extern unsigned char __lab_data_start[];
extern unsigned char __lab_data_end[];
extern unsigned char __lab_bss_start[];
extern unsigned char __lab_bss_end[];
extern unsigned char __reloc_image_start[];
extern unsigned char __reloc_image_end[];

#endif


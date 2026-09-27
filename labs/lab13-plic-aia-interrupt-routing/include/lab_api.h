#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>
#include "lab_layout.h"

struct lab_plic_context_entry {
    uint64_t hartid;
    uint64_t machine_context;
    uint64_t supervisor_context;
};

uint64_t lab_plic_context_lookup(uint64_t hartid, uint64_t privilege,
                                 const struct lab_plic_context_entry *table,
                                 uint64_t count);
uint64_t lab_plic_select(const uint32_t *priorities, uint64_t pending,
                         uint64_t enabled, uint64_t threshold,
                         uint64_t source_count);
uint64_t lab_plic_service(volatile uint32_t *claim_complete,
                          volatile uint8_t *device_ack, uint64_t ack_value,
                          uint64_t max_source);
int lab_plic_retarget(uint64_t *old_enable, uint64_t *new_enable,
                      uint64_t source, uint64_t active_handlers);
uint64_t lab_aplic_direct_target(uint64_t hart_index, uint64_t priority,
                                 uint64_t hart_count, uint64_t priority_max);
uint64_t lab_aplic_msi_target(uint64_t hart_index, uint64_t guest_index,
                              uint64_t eiid, uint64_t hart_count,
                              uint64_t guest_files, uint64_t identity_count);
uint64_t lab_imsic_guest_allocate(volatile uint64_t *allocation_bitmap,
                                  uint64_t guest_files,
                                  uint64_t preferred_guest);
uint64_t lab_indirect_window_access(volatile uint32_t *selector_register,
                                    volatile uint32_t *data_register,
                                    uint64_t selector, uint64_t value,
                                    uint64_t operation);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
void lab_smoke_main(uint64_t hartid) __attribute__((noreturn));
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

extern unsigned char __global_pointer$[], __bss_start[], __bss_end[];
extern unsigned char __stack_bottom[], __stack_top[];

#endif

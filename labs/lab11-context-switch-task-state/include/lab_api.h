#ifndef LAB_API_H
#define LAB_API_H
#include <stdint.h>
#include "lab_layout.h"

struct lab_switch_context {
    uint64_t ra, sp;
    uint64_t s[12];
    uint64_t tp;
    uint64_t state;
};

int lab_context_init(struct lab_switch_context *context,
                     uintptr_t stack_base, uint64_t stack_size,
                     const void *entry, uint64_t argument,
                     const void *exit_entry, uint64_t task_tp);
void lab_context_switch(struct lab_switch_context *outgoing,
                        const struct lab_switch_context *incoming);
uint64_t lab_vector_context_size(uint64_t vlenb);
uint64_t lab_extension_switch_plan(uint64_t outgoing_state,
                                   uint64_t incoming_state,
                                   uint64_t incoming_owner_mask);
int lab_migration_allowed(uint64_t required_isa, uint64_t required_vlenb,
                          uint64_t task_flags, uint64_t target_isa,
                          uint64_t target_vlenb, uint64_t target_flags);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
unsigned int lab_run_unit_tests(void);
void lab_task_trampoline(void) __attribute__((noreturn));
uint64_t lab_task_probe(uint64_t argument);
void lab_task_exit(uint64_t result) __attribute__((noreturn));
void lab_task_unexpected_return(void) __attribute__((noreturn));
void *lab_memzero(void *destination, uint64_t size);
void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern struct lab_switch_context lab_scheduler_context;
extern struct lab_switch_context lab_task_context;
extern volatile uint64_t lab_task_phase;
extern volatile uint64_t lab_task_result;
extern volatile uint64_t lab_task_probe_failed;
extern unsigned char __global_pointer$[], __bss_start[], __bss_end[];
extern unsigned char __stack_top[], __task_stack_bottom[], __task_stack_top[];
#endif

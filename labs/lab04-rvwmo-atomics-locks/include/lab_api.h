#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

struct lab_spinlock {
    volatile uint32_t value;
    uint32_t padding;
};

struct lab_ticketlock {
    volatile uint32_t next;
    volatile uint32_t owner;
};

struct lab_mailbox {
    volatile uint64_t data;
    volatile uint64_t ready;
};

uint64_t lab_atomic_fetch_add(volatile uint64_t *address, uint64_t increment);
int lab_spin_trylock(struct lab_spinlock *lock);
void lab_spin_lock(struct lab_spinlock *lock);
void lab_spin_unlock(struct lab_spinlock *lock);
void lab_ticket_lock(struct lab_ticketlock *lock);
void lab_ticket_unlock(struct lab_ticketlock *lock);
void lab_publish_message(struct lab_mailbox *mailbox, uint64_t value);
int lab_consume_message(struct lab_mailbox *mailbox, uint64_t *output);
uint64_t lab_atomic_test_and_set_bit(volatile uint64_t *word,
                                     uint64_t bit_index);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
void lab_stress_main(uint64_t hartid) __attribute__((noreturn));
unsigned int lab_run_unit_tests(void);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern unsigned char __global_pointer$[];
extern unsigned char __emergency_stacks_start[];
extern unsigned char __hart_stacks_start[];
extern unsigned char __per_hart_start[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];
extern volatile uint64_t lab_boot_ready;
extern volatile uint64_t lab_smoke_arrived;

#endif

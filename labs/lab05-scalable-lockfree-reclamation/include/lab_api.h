#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

struct lab_mcs_node {
    volatile uintptr_t next;
    volatile uint32_t locked;
    uint32_t reserved;
    uint8_t padding[48];
} __attribute__((aligned(LAB_CACHELINE_SIZE)));

struct lab_mcs_lock {
    volatile uintptr_t tail;
    uint8_t padding[56];
} __attribute__((aligned(LAB_CACHELINE_SIZE)));

struct lab_spsc_ring {
    volatile uint64_t head;
    uint8_t head_padding[56];
    volatile uint64_t tail;
    uint8_t tail_padding[56];
    volatile uint64_t slots[LAB_SPSC_CAPACITY];
} __attribute__((aligned(LAB_CACHELINE_SIZE)));

struct lab_seq_data {
    volatile uint64_t sequence;
    volatile uint64_t x;
    volatile uint64_t y;
};

struct lab_stack_node {
    volatile uintptr_t next;
    uint64_t value;
};

struct lab_stack {
    volatile uintptr_t head;
};

struct lab_qsbr {
    volatile uint64_t global_epoch;
    uint8_t global_padding[56];
    volatile uint64_t observed[LAB_HART_COUNT];
    uint8_t observed_padding[32];
} __attribute__((aligned(LAB_CACHELINE_SIZE)));

void lab_mcs_lock(struct lab_mcs_lock *lock, struct lab_mcs_node *node);
void lab_mcs_unlock(struct lab_mcs_lock *lock, struct lab_mcs_node *node);

int lab_spsc_try_push(struct lab_spsc_ring *ring, uint64_t value);
int lab_spsc_try_pop(struct lab_spsc_ring *ring, uint64_t *output);

void lab_seq_write_begin(struct lab_seq_data *data);
void lab_seq_write_end(struct lab_seq_data *data);
uint64_t lab_seq_read_begin(struct lab_seq_data *data);
int lab_seq_read_retry(struct lab_seq_data *data, uint64_t start);

void lab_stack_push(struct lab_stack *stack, struct lab_stack_node *node);
struct lab_stack_node *lab_stack_pop_protected(
    struct lab_stack *stack, volatile uintptr_t *hazard_slot);
void lab_hazard_clear(volatile uintptr_t *hazard_slot);
int lab_hazard_is_protected(volatile uintptr_t *hazards, uint64_t count,
                            struct lab_stack_node *candidate);

uint64_t lab_qsbr_start(struct lab_qsbr *domain);
void lab_qsbr_report(struct lab_qsbr *domain, uint64_t hartid);
int lab_qsbr_complete(struct lab_qsbr *domain, uint64_t target_epoch);

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

#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

struct lab_cmo_plan {
    uint64_t start;
    uint64_t end;
    uint64_t blocks;
    uint64_t head_partial;
    uint64_t tail_partial;
};

struct lab_trace_event {
    uint64_t operation;
    uint64_t address;
};

struct lab_dma_desc {
    volatile uint64_t address;
    volatile uint64_t length;
    volatile uint64_t owner;
    volatile uint64_t status;
    uint8_t padding[32];
} __attribute__((aligned(LAB_CACHE_BLOCK_SIZE)));

struct lab_dma_channel {
    volatile uint64_t doorbell;
    volatile uint64_t completion;
    uint8_t padding[48];
} __attribute__((aligned(LAB_CACHE_BLOCK_SIZE)));

int lab_normalize_cmo_range(uint64_t address, uint64_t size,
                            uint64_t block_size,
                            struct lab_cmo_plan *plan);
int lab_cmo_apply(uint64_t address, uint64_t size, uint64_t block_size,
                  uint64_t operation, uint64_t exclusive);
int lab_dma_prepare_tx(struct lab_dma_channel *channel,
                       struct lab_dma_desc *descriptor,
                       const void *buffer, uint64_t size,
                       uint64_t block_size);
int lab_dma_complete_rx(struct lab_dma_channel *channel,
                        struct lab_dma_desc *descriptor,
                        void *buffer, uint64_t size,
                        uint64_t block_size);
int lab_publish_code(uint32_t *destination, const uint32_t *source,
                     uint64_t word_count);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
unsigned int lab_run_unit_tests(void);
uint64_t lab_call_generated(const void *address);

void *lab_memcpy(void *destination, const void *source, uint64_t size);
void *lab_memzero(void *destination, uint64_t size);
void lab_trace_reset(void);
int lab_cmo_emit(uint64_t operation, uint64_t address);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern volatile struct lab_trace_event
    lab_trace_events[LAB_TRACE_MAX_EVENTS];
extern volatile uint64_t lab_trace_count;

extern unsigned char __global_pointer$[];
extern unsigned char __stack_top[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];
extern unsigned char __jit_buffer_start[];
extern unsigned char __jit_buffer_end[];

#endif

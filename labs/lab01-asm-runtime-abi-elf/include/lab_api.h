#ifndef LAB_API_H
#define LAB_API_H

#include <stddef.h>
#include <stdint.h>

#define LAB_CHECKSUM_INIT UINT64_C(0xcbf29ce484222325)
#define LAB_GLOBAL_VALUE UINT64_C(0x13579bdf2468ace0)

void asm_memzero(void *start, size_t size);
void *asm_memmove(void *dst, const void *src, size_t size);
void asm_add128(uint64_t out[2], const uint64_t lhs[2],
                const uint64_t rhs[2]);
uint64_t asm_call_checksum(const uint64_t *values, size_t count);
uintptr_t asm_global_address(void);
uint64_t asm_global_load(void);
uint64_t asm_pic_blob(uint64_t value);

uint64_t lab_checksum_step(uint64_t accumulator, uint64_t value);
uint64_t lab_check_callee_saved(uint64_t (*function)(const uint64_t *, size_t),
                                const uint64_t *values, size_t count,
                                uint64_t *result);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_fatal_trap(uint64_t cause, uint64_t epc, uint64_t trap_value)
    __attribute__((noreturn));

extern const uint64_t lab_global_anchor;
extern const unsigned char __lab_blob_start[];
extern const unsigned char __lab_blob_end[];

#endif


#include "lab_api.h"

#define UART_BASE ((uintptr_t)0x10000000)
#define TEST_FINISHER ((volatile uint32_t *)(uintptr_t)0x100000)

const uint64_t lab_platform_hartids[LAB_HART_COUNT] = { 0, 1, 2, 3 };
static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

void lab_putc(char character)
{
    while ((uart[5] & 0x20u) == 0u) {
    }
    uart[0] = (uint8_t)character;
}

void lab_puts(const char *string)
{
    while (*string != '\0') {
        if (*string == '\n') lab_putc('\r');
        lab_putc(*string++);
    }
}

void lab_puthex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    unsigned int shift;
    lab_puts("0x");
    for (shift = 60; shift < 64; shift -= 4) {
        lab_putc(digits[(value >> shift) & 15u]);
        if (shift == 0) break;
    }
}

void lab_exit(unsigned int status)
{
    *TEST_FINISHER = status ? UINT32_C(0x13333) : UINT32_C(0x5555);
    for (;;) __asm__ volatile ("wfi");
}

unsigned int lab_report(const char *name, int passed)
{
    lab_puts(passed ? "[PASS] " : "[FAIL] ");
    lab_puts(name);
    lab_puts("\n");
    return passed ? 0u : 1u;
}

uint64_t lab_read_mtime(void)
{
    return *(volatile uint64_t *)(uintptr_t)(LAB_CLINT_BASE +
                                             LAB_CLINT_MTIME_OFFSET);
}

volatile uint32_t *lab_msip(uint64_t hartid)
{
    return (volatile uint32_t *)(uintptr_t)(LAB_CLINT_BASE +
        LAB_CLINT_MSIP_OFFSET + 4u * hartid);
}

volatile uint64_t *lab_mtimecmp(uint64_t hartid)
{
    return (volatile uint64_t *)(uintptr_t)(LAB_CLINT_BASE +
        LAB_CLINT_MTIMECMP_OFFSET + 8u * hartid);
}

__attribute__((weak)) void lab_trap_dispatch(uint64_t hartid, uint64_t mcause)
{
    (void)hartid;
    lab_puts("UNEXPECTED TRAP: ");
    lab_puthex(mcause);
    lab_puts("\n");
    lab_exit(1);
}

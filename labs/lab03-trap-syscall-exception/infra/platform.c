#include "lab_api.h"

#define UART_BASE ((uintptr_t)0x10000000)
#define UART_THR 0u
#define UART_LSR 5u
#define UART_LSR_THRE 0x20u

static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

void lab_putc(char character)
{
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0u) {
    }
    uart[UART_THR] = (uint8_t)character;
}

void lab_puts(const char *string)
{
    while (*string != '\0') {
        if (*string == '\n') {
            lab_putc('\r');
        }
        lab_putc(*string++);
    }
}

void lab_puthex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    unsigned int shift;
    lab_puts("0x");
    for (shift = 60; shift < 64; shift -= 4) {
        lab_putc(digits[(value >> shift) & 0xfu]);
        if (shift == 0) {
            break;
        }
    }
}

void lab_exit(unsigned int status)
{
    uint32_t value = status == 0 ? UINT32_C(0x5555) : UINT32_C(0x13333);
    *(volatile uint32_t *)(uintptr_t)0x100000 = value;
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

unsigned int lab_report(const char *name, int passed)
{
    lab_puts(passed ? "[PASS] " : "[FAIL] ");
    lab_puts(name);
    lab_puts("\n");
    return passed ? 0u : 1u;
}

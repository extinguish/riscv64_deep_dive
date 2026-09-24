#include "lab_api.h"
#define UART_BASE ((uintptr_t)0x10000000)
static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;
void lab_putc(char c) { while ((uart[5] & 0x20u) == 0u) {} uart[0] = (uint8_t)c; }
void lab_puts(const char *s) { while (*s) { if (*s == '\n') lab_putc('\r'); lab_putc(*s++); } }
void lab_puthex(uint64_t v) {
    static const char d[] = "0123456789abcdef"; unsigned int shift;
    lab_puts("0x");
    for (shift = 60; shift < 64; shift -= 4) { lab_putc(d[(v >> shift) & 15u]); if (!shift) break; }
}
void lab_exit(unsigned int status) {
    *(volatile uint32_t *)(uintptr_t)0x100000 = status ? UINT32_C(0x13333) : UINT32_C(0x5555);
    for (;;) __asm__ volatile ("wfi");
}
unsigned int lab_report(const char *name, int passed) {
    lab_puts(passed ? "[PASS] " : "[FAIL] "); lab_puts(name); lab_puts("\n");
    return passed ? 0u : 1u;
}

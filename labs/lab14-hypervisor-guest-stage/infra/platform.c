#include "lab_api.h"

static volatile uint8_t *const uart = (volatile uint8_t *)(uintptr_t)0x10000000;
static volatile uint32_t *const finisher = (volatile uint32_t *)(uintptr_t)0x100000;

static void putc(char c)
{
    while ((uart[5] & 0x20u) == 0u) {}
    uart[0] = (uint8_t)c;
}

void lab_puts(const char *s)
{
    while (*s) { if (*s == '\n') putc('\r'); putc(*s++); }
}

void lab_puthex(uint64_t value)
{
    static const char hex[] = "0123456789abcdef";
    lab_puts("0x");
    for (int i = 60; i >= 0; i -= 4) putc(hex[(value >> i) & 15u]);
}

unsigned lab_report(const char *name, int ok)
{
    lab_puts(ok ? "[PASS] " : "[FAIL] ");
    lab_puts(name);
    lab_puts("\n");
    return !ok;
}

void lab_exit(unsigned status)
{
    *finisher = status ? 0x13333u : 0x5555u;
    for (;;) __asm__ volatile ("wfi");
}

void lab_m_fatal(void)
{
    uint64_t cause, epc, tval;
    __asm__ volatile ("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile ("csrr %0, mepc" : "=r"(epc));
    __asm__ volatile ("csrr %0, mtval" : "=r"(tval));
    lab_puts("M FATAL mcause="); lab_puthex(cause); lab_puts("\n");
    lab_puts("mepc="); lab_puthex(epc); lab_puts(" mtval=");
    lab_puthex(tval); lab_puts("\n");
    lab_exit(1);
}

void lab_no_h(void)
{
    lab_puts("H extension absent; use QEMU -cpu rv64,h=true\n");
    lab_exit(1);
}

void lab_bad_return(void)
{
    lab_puts("HS trap dispatcher returned unexpectedly\n");
    lab_exit(1);
}

#include "lab_api.h"

static volatile uint64_t bss_marker;
static volatile uint64_t data_marker = UINT64_C(0x1122334455667788);

static int report(const char *name, int passed)
{
    lab_puts(passed ? "[PASS] " : "[FAIL] ");
    lab_puts(name);
    lab_puts("\n");
    return passed ? 0 : 1;
}

int main(void)
{
    uintptr_t stack_pointer;
    int failures = 0;

    __asm__ volatile ("mv %0, sp" : "=r"(stack_pointer));
    lab_puts("LAB01 infrastructure smoke test\n");
    failures += report("UART output", 1);
    failures += report(".bss was cleared", bss_marker == 0);
    failures += report(".data is readable",
                       data_marker == UINT64_C(0x1122334455667788));
    failures += report("stack is 16-byte aligned", (stack_pointer & 15u) == 0u);
    lab_puts(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL\n");
    return failures == 0 ? 0 : 1;
}


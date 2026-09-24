#include "lab_api.h"
void lab_smoke_main(uint64_t hartid)
{
    unsigned int failures = 0;
    uint8_t bytes[16];
    uintptr_t bottom = (uintptr_t)__task_stack_bottom;
    uintptr_t top = (uintptr_t)__task_stack_top;
    uint64_t i;
    int zero = 1;
    (void)hartid;
    lab_memzero(bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); ++i) if (bytes[i] != 0) zero = 0;
    lab_puts("LAB11 infrastructure smoke tests\n");
    failures += lab_report("dedicated aligned task stack",
        (bottom & 0xfffu) == 0 && top - bottom == LAB_STACK_SIZE);
    failures += lab_report("task trampoline and memory helper",
        (uintptr_t)lab_task_trampoline != 0 && zero);
    lab_puts(failures ? "SMOKE FAIL\n" : "SMOKE PASS\n");
    lab_exit(failures ? 1u : 0u);
}

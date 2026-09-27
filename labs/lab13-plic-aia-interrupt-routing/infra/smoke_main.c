#include "lab_api.h"

void lab_smoke_main(uint64_t hartid)
{
    volatile uint32_t *priority = (volatile uint32_t *)(uintptr_t)
        (LAB_PLIC_BASE + 4u * LAB_UART_SOURCE);
    volatile uint32_t *threshold = (volatile uint32_t *)(uintptr_t)
        (LAB_PLIC_BASE + LAB_PLIC_CONTEXT_M0_OFFSET +
         LAB_PLIC_THRESHOLD_OFFSET);
    uint32_t old_priority = *priority;
    uint32_t old_threshold = *threshold;
    uintptr_t stack_bottom = (uintptr_t)__stack_bottom;
    uintptr_t stack_top = (uintptr_t)__stack_top;
    uintptr_t stack_pointer;
    unsigned int failures = 0;
    (void)hartid;

    __asm__ volatile ("mv %0, sp" : "=r"(stack_pointer));
    *priority = 1;
    *threshold = 0;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
    lab_puts("LAB13 infrastructure smoke tests\n");
    failures += lab_report("PLIC priority and threshold MMIO",
                           *priority == 1 && *threshold == 0);
    failures += lab_report("aligned runtime stack",
                           stack_pointer >= stack_bottom &&
                           stack_pointer <= stack_top &&
                           (stack_pointer & 15u) == 0);
    *priority = old_priority;
    *threshold = old_threshold;
    lab_puts(failures ? "SMOKE FAIL\n" : "SMOKE PASS\n");
    lab_exit(failures ? 1u : 0u);
}

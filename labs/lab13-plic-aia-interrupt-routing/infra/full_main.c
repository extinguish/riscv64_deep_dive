#include "lab_api.h"

static volatile uint64_t external_seen;
static volatile uint64_t external_source;
static volatile uint64_t external_count;
static volatile uint64_t unexpected_trap;

static volatile uint32_t *plic_priority(uint64_t source)
{
    return (volatile uint32_t *)(uintptr_t)(LAB_PLIC_BASE + 4u * source);
}

static volatile uint32_t *plic_enable(void)
{
    return (volatile uint32_t *)(uintptr_t)(LAB_PLIC_BASE +
                                            LAB_PLIC_ENABLE_M0_OFFSET);
}

static volatile uint32_t *plic_threshold(void)
{
    return (volatile uint32_t *)(uintptr_t)(LAB_PLIC_BASE +
        LAB_PLIC_CONTEXT_M0_OFFSET + LAB_PLIC_THRESHOLD_OFFSET);
}

static volatile uint32_t *plic_claim_complete(void)
{
    return (volatile uint32_t *)(uintptr_t)(LAB_PLIC_BASE +
        LAB_PLIC_CONTEXT_M0_OFFSET + LAB_PLIC_CLAIM_OFFSET);
}

static volatile uint8_t *uart_ier(void)
{
    return (volatile uint8_t *)(uintptr_t)(LAB_UART_BASE +
                                            LAB_UART_IER_OFFSET);
}

static void disable_external_interrupts(void)
{
    __asm__ volatile ("csrc mie, %0" : : "r"(LAB_MIE_MEIE) : "memory");
    __asm__ volatile ("csrc mstatus, %0" : : "r"(LAB_MSTATUS_MIE) : "memory");
}

void lab_trap_dispatch(uint64_t hartid, uint64_t mcause)
{
    uint64_t source;
    if (hartid != 0 || (mcause & LAB_MCAUSE_INTERRUPT) == 0 ||
        (mcause & LAB_MCAUSE_CODE_MASK) != LAB_MCAUSE_MEI) {
        unexpected_trap = mcause | 1;
        lab_exit(1);
    }
    source = lab_plic_service(plic_claim_complete(), uart_ier(), 0,
                              LAB_PLIC_MAX_SOURCE);
    external_source = source;
    external_count++;
    external_seen = 1;
}

static int run_external_interrupt(void)
{
    uint32_t enable = *plic_enable();
    int passed;

    disable_external_interrupts();
    *uart_ier() = 0;
    *plic_priority(LAB_UART_SOURCE) = 1;
    *plic_enable() = enable | (UINT32_C(1) << LAB_UART_SOURCE);
    *plic_threshold() = 0;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");

    __asm__ volatile ("csrs mie, %0" : : "r"(LAB_MIE_MEIE) : "memory");
    __asm__ volatile ("csrs mstatus, %0" : : "r"(LAB_MSTATUS_MIE) : "memory");
    *uart_ier() = LAB_UART_IER_THRI;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
    while (external_seen == 0) __asm__ volatile ("wfi");

    disable_external_interrupts();
    *uart_ier() = 0;
    *plic_enable() = enable & ~(UINT32_C(1) << LAB_UART_SOURCE);
    *plic_priority(LAB_UART_SOURCE) = 0;
    *plic_threshold() = 0;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
    passed = external_source == LAB_UART_SOURCE && external_count == 1 &&
             unexpected_trap == 0 && *uart_ier() == 0 &&
             *plic_claim_complete() == 0;
    return passed;
}

void lab_full_main(uint64_t hartid)
{
    unsigned int failures;
    (void)hartid;
    lab_puts("LAB13 real PLIC external-interrupt tests\n");
    failures = lab_run_unit_tests();
    if (failures != 0) {
        failures += lab_report("real UART-to-PLIC phase enabled", 0);
        lab_puts("LAB13 INCOMPLETE\n");
        lab_exit(1);
    }
    failures += lab_report("UART THRE -> PLIC claim/ack/complete",
                           run_external_interrupt());
    lab_puts(failures ? "LAB13 INCOMPLETE\n" : "LAB13 PASS\n");
    lab_exit(failures ? 1u : 0u);
}

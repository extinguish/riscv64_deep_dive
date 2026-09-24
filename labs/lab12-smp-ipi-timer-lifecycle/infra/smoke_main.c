#include "lab_api.h"

static volatile uint64_t smoke_arrived;
static volatile uint64_t smoke_seen;
static volatile uint64_t smoke_bad_stack;
static volatile uint64_t smoke_bad_tp;

static uint64_t atomic_add(volatile uint64_t *address, uint64_t value)
{
    uint64_t old;
    __asm__ volatile ("amoadd.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(address), "r"(value) : "memory");
    return old;
}

static void atomic_or(volatile uint64_t *address, uint64_t value)
{
    __asm__ volatile ("amoor.d.aqrl zero, %1, (%0)"
                      : : "r"(address), "r"(value) : "memory");
}

static uint64_t load_acquire(const volatile uint64_t *address)
{
    uint64_t value = *address;
    __asm__ volatile ("fence r, rw" ::: "memory");
    return value;
}

void lab_smoke_main(uint64_t hartid, uintptr_t stack_pointer,
                    uintptr_t thread_pointer)
{
    uintptr_t stack_bottom = (uintptr_t)__hart_stacks_start +
        hartid * LAB_HART_STACK_SIZE;
    uintptr_t stack_top = stack_bottom + LAB_HART_STACK_SIZE;
    uintptr_t expected_tp = (uintptr_t)__per_hart_start +
        hartid * LAB_PER_HART_SIZE;
    uint64_t start;
    unsigned int failures = 0;

    if (stack_pointer < stack_bottom || stack_pointer > stack_top ||
        (stack_pointer & 15u) != 0u) {
        atomic_or(&smoke_bad_stack, UINT64_C(1) << hartid);
    }
    if (thread_pointer != expected_tp) {
        atomic_or(&smoke_bad_tp, UINT64_C(1) << hartid);
    }
    atomic_or(&smoke_seen, UINT64_C(1) << hartid);
    atomic_add(&smoke_arrived, 1);

    if (hartid != 0) {
        for (;;) __asm__ volatile ("wfi");
    }

    start = lab_read_mtime();
    while (load_acquire(&smoke_arrived) != LAB_HART_COUNT &&
           lab_read_mtime() - start < LAB_WAIT_TICKS) {
    }
    lab_puts("LAB12 infrastructure smoke tests\n");
    failures += lab_report("all four QEMU harts entered",
                           smoke_arrived == LAB_HART_COUNT &&
                           smoke_seen == 0xf);
    failures += lab_report("isolated aligned stacks and per-hart tp",
                           smoke_bad_stack == 0 && smoke_bad_tp == 0);
    failures += lab_report("CLINT mtime is advancing",
                           lab_read_mtime() > start);
    lab_puts(failures ? "SMOKE FAIL\n" : "SMOKE PASS\n");
    lab_exit(failures ? 1u : 0u);
}

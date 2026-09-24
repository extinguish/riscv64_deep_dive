#include "lab_api.h"

static int test_stack_regions(void)
{
    uintptr_t trap_bottom = (uintptr_t)__trap_stack_bottom;
    uintptr_t trap_top = (uintptr_t)__trap_stack_top;
    uintptr_t user_bottom = (uintptr_t)__user_stack_bottom;
    uintptr_t user_top = (uintptr_t)__user_stack_top;
    uintptr_t emergency_bottom = (uintptr_t)__emergency_stack_bottom;
    uintptr_t emergency_top = (uintptr_t)__emergency_stack_top;

    return (trap_bottom & 0xfffu) == 0 &&
           (user_bottom & 0xfffu) == 0 &&
           (emergency_bottom & 0xfffu) == 0 &&
           trap_top - trap_bottom == LAB_STACK_SIZE &&
           user_top - user_bottom == LAB_STACK_SIZE &&
           emergency_top - emergency_bottom == LAB_EMERGENCY_STACK_SIZE &&
           trap_top <= user_bottom && user_top <= emergency_bottom;
}

static int test_vector_geometry(void)
{
    uintptr_t base = (uintptr_t)lab_vector_example;
    uintptr_t end = (uintptr_t)lab_vector_example_end;

    return (base & 0x7fu) == 0 && end - base == 16u * 4u;
}

void lab_smoke_main(uint64_t hartid)
{
    unsigned int failures = 0;
    uint8_t bytes[16];
    uint64_t index;
    int zeroed = 1;
    (void)hartid;

    lab_memzero(bytes, sizeof(bytes));
    for (index = 0; index < sizeof(bytes); ++index) {
        if (bytes[index] != 0) {
            zeroed = 0;
        }
    }
    lab_puts("LAB10 infrastructure smoke tests\n");
    failures += lab_report("separate aligned trap/user/emergency stacks",
                           test_stack_regions());
    failures += lab_report("fixed four-byte vectored table slots",
                           test_vector_geometry());
    failures += lab_report("stack and BSS memory helper", zeroed);
    lab_puts(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

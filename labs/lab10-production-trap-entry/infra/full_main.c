#include "lab_api.h"

volatile uint64_t lab_full_failures;

void lab_full_main(uint64_t hartid)
{
    uintptr_t vector = (uintptr_t)lab_s_trap_vector;
    (void)hartid;

    lab_puts("LAB10 production S-mode trap-entry tests\n");
    lab_full_failures = lab_run_unit_tests();
    if ((vector & 3u) != 0) {
        lab_full_failures += lab_report("Direct stvec alignment", 0);
        lab_puts("LAB10 INCOMPLETE\n");
        lab_exit(1);
    }
    __asm__ volatile ("csrw stvec, %0" :: "r"(vector) : "memory");
    __asm__ volatile ("csrw sscratch, %0" ::
                      "r"((uintptr_t)__trap_stack_top) : "memory");
    lab_puts("Entering U-mode ecall/breakpoint probe\n");
    lab_enter_user(lab_user_probe, (uintptr_t)__user_stack_top);
}

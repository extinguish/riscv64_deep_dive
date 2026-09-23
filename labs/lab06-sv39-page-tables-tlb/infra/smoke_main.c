#include "lab_api.h"

static void lab_smoke_supervisor_main(void) __attribute__((noreturn));

void lab_smoke_machine_main(void)
{
    lab_prepare_page_tables(1);
    lab_enter_supervisor((uintptr_t)lab_smoke_supervisor_main,
                         lab_infra_satp(lab_mmu_root, 0),
                         (uintptr_t)lab_s_trap_vector);
}

static void lab_smoke_supervisor_main(void)
{
    uint64_t satp;
    uint64_t value = *(volatile uint64_t *)(uintptr_t)LAB_TEST_VA;
    __asm__ volatile ("csrr %0, satp" : "=r"(satp));

    if ((satp >> LAB_SATP_MODE_SHIFT) != LAB_SATP_MODE_SV39 ||
        value != UINT64_C(0x1111222233334444)) {
        lab_puts("LAB06 infrastructure smoke failed\n");
        lab_exit(1);
    }
    lab_puts("LAB06 infrastructure smoke: Sv39 S-mode alias works\n");
    lab_puts("SMOKE PASS\n");
    lab_exit(0);
}

#include "lab_api.h"

void lab_smoke_main(uint64_t hartid)
{
    uint64_t value = 0;
    (void)hartid;

    *(uint64_t *)(void *)lab_pmp_test_page = UINT64_C(0x504d50534d4f4b45);
    lab_pmp_baseline_init();
    if (!lab_try_mprv_load64((uintptr_t)lab_pmp_test_page, &value) ||
        value != UINT64_C(0x504d50534d4f4b45) || lab_fault_count != 0) {
        lab_puts("LAB07 infrastructure smoke failed\n");
        lab_exit(1);
    }
    lab_puts("LAB07 infrastructure smoke: PMP baseline and MPRV load work\n");
    lab_puts("SMOKE PASS\n");
    lab_exit(0);
}

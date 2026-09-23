#include "lab_api.h"

static uint64_t read_pmpaddr0(void)
{
    uint64_t value;
    __asm__ volatile ("csrr %0, pmpaddr0" : "=r"(value));
    return value;
}

static uint64_t read_pmpcfg0(void)
{
    uint64_t value;
    __asm__ volatile ("csrr %0, pmpcfg0" : "=r"(value));
    return value;
}

static void clear_fault_state(void)
{
    lab_fault_count = 0;
    lab_fault_cause = 0;
    lab_fault_value = 0;
}

static int test_deny_window(void)
{
    uint64_t value = 0;
    uint64_t base = (uintptr_t)lab_pmp_test_page;
    uint64_t expected_address = (base >> 2) |
                                ((LAB_PAGE_SIZE >> 3) - 1);

    lab_pmp_baseline_init();
    if (!lab_pmp_program_window(base, LAB_PAGE_SIZE, 0) ||
        read_pmpaddr0() != expected_address ||
        (read_pmpcfg0() & UINT64_C(0xff)) != UINT64_C(0x18)) {
        return 0;
    }
    clear_fault_state();
    if (lab_try_mprv_load64(base, &value) != 0 ||
        lab_fault_count != 1 ||
        lab_fault_cause != LAB_CAUSE_LOAD_ACCESS_FAULT ||
        lab_fault_value != base) {
        return 0;
    }
    if (lab_try_mprv_store64(base, UINT64_C(0x55)) != 0 ||
        lab_fault_count != 2 ||
        lab_fault_cause != LAB_CAUSE_STORE_ACCESS_FAULT ||
        lab_fault_value != base) {
        return 0;
    }
    return 1;
}

static int test_read_only_window(void)
{
    uint64_t value = 0;
    uint64_t base = (uintptr_t)lab_pmp_test_page;

    lab_pmp_baseline_init();
    if (!lab_pmp_program_window(base, LAB_PAGE_SIZE, LAB_PMP_R) ||
        (read_pmpcfg0() & UINT64_C(0xff)) != UINT64_C(0x19)) {
        return 0;
    }
    clear_fault_state();
    if (!lab_try_mprv_load64(base, &value) ||
        value != UINT64_C(0x1122334455667788) || lab_fault_count != 0) {
        return 0;
    }
    if (lab_try_mprv_store64(base, UINT64_C(0x99)) != 0 ||
        lab_fault_count != 1 ||
        lab_fault_cause != LAB_CAUSE_STORE_ACCESS_FAULT ||
        lab_fault_value != base) {
        return 0;
    }
    return 1;
}

static int test_read_write_and_disable(void)
{
    uint64_t value = 0;
    uint64_t base = (uintptr_t)lab_pmp_test_page;

    lab_pmp_baseline_init();
    if (!lab_pmp_program_window(base, LAB_PAGE_SIZE,
                                LAB_PMP_R | LAB_PMP_W) ||
        (read_pmpcfg0() & UINT64_C(0xff)) != UINT64_C(0x1b)) {
        return 0;
    }
    clear_fault_state();
    if (!lab_try_mprv_store64(base, UINT64_C(0xcafebabedeadbeef)) ||
        !lab_try_mprv_load64(base, &value) ||
        value != UINT64_C(0xcafebabedeadbeef) || lab_fault_count != 0) {
        return 0;
    }
    if (!lab_pmp_disable_window() ||
        (read_pmpcfg0() & UINT64_C(0xff)) != 0 ||
        !lab_try_mprv_load64(base, &value)) {
        return 0;
    }
    return 1;
}

void lab_full_main(uint64_t hartid)
{
    unsigned int failures = 0;
    (void)hartid;

    *(uint64_t *)(void *)lab_pmp_test_page = UINT64_C(0x1122334455667788);
    lab_puts("LAB07 live PMP policy tests\n");
    failures += lab_report("entry-0 deny overrides allow-all baseline",
                           test_deny_window());
    failures += lab_report("read-only window and store access fault",
                           test_read_only_window());
    failures += lab_report("read-write window and safe disable",
                           test_read_write_and_disable());
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB07 PASS\n" : "LAB07 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

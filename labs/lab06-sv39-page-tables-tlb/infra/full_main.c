#include "lab_api.h"

static void lab_full_supervisor_main(void) __attribute__((noreturn));

void lab_full_machine_main(void)
{
    lab_prepare_page_tables(0);
    lab_enter_supervisor((uintptr_t)lab_full_supervisor_main,
                         lab_infra_satp(lab_mmu_root, 0),
                         (uintptr_t)lab_s_trap_vector);
}

static int test_live_mapping(void)
{
    const uint64_t flags = LAB_PTE_R | LAB_PTE_W | LAB_PTE_A | LAB_PTE_D;
    uint64_t value = 0;

    if (!lab_install_4k(lab_mmu_root, LAB_TEST_VA,
                        (uintptr_t)lab_test_page_a, flags) ||
        !lab_try_load64(LAB_TEST_VA, &value) ||
        value != UINT64_C(0x1111222233334444)) {
        return 0;
    }
    if (!lab_try_store64(LAB_TEST_VA, UINT64_C(0x0123456789abcdef)) ||
        *(uint64_t *)(void *)lab_test_page_a != UINT64_C(0x0123456789abcdef)) {
        return 0;
    }
    if (!lab_install_4k(lab_mmu_root, LAB_TEST_VA,
                        (uintptr_t)lab_test_page_b, flags) ||
        !lab_try_load64(LAB_TEST_VA, &value) ||
        value != UINT64_C(0xaaaabbbbccccdddd)) {
        return 0;
    }
    return 1;
}

static int test_permission_faults(void)
{
    const uint64_t read_only = LAB_PTE_R | LAB_PTE_A;
    uint64_t value = 0;

    lab_fault_count = 0;
    lab_fault_cause = 0;
    lab_fault_value = 0;
    if (!lab_install_4k(lab_mmu_root, LAB_TEST_VA,
                        (uintptr_t)lab_test_page_a, read_only) ||
        !lab_try_load64(LAB_TEST_VA, &value)) {
        return 0;
    }
    if (lab_try_store64(LAB_TEST_VA, UINT64_C(0xdeadbeef)) != 0 ||
        lab_fault_count != 1 ||
        lab_fault_cause != LAB_CAUSE_STORE_PAGE_FAULT ||
        lab_fault_value != LAB_TEST_VA) {
        return 0;
    }
    if (!lab_unmap_4k(lab_mmu_root, LAB_TEST_VA) ||
        lab_try_load64(LAB_TEST_VA, &value) != 0 ||
        lab_fault_count != 2 ||
        lab_fault_cause != LAB_CAUSE_LOAD_PAGE_FAULT ||
        lab_fault_value != LAB_TEST_VA) {
        return 0;
    }
    return 1;
}

static int test_satp_switch(void)
{
    uint64_t actual;
    uint64_t expected = lab_infra_satp(lab_mmu_root, 7);

    if (!lab_switch_address_space(lab_mmu_root, 7)) {
        return 0;
    }
    __asm__ volatile ("csrr %0, satp" : "=r"(actual));
    return actual == expected;
}

static void lab_full_supervisor_main(void)
{
    unsigned int failures = 0;

    lab_puts("LAB06 Sv39 live translation tests\n");
    failures += lab_report("live 4 KiB map/remap and local TLB invalidation",
                           test_live_mapping());
    failures += lab_report("read-only and unmapped page faults",
                           test_permission_faults());
    failures += lab_report("satp root/ASID switch",
                           test_satp_switch());
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB06 PASS\n" : "LAB06 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

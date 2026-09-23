#include "lab_api.h"

static void zero_walk_tables(void)
{
    uint64_t index;
    for (index = 0; index < LAB_PT_ENTRIES; ++index) {
        lab_walk_root[index] = 0;
        lab_walk_l1[index] = 0;
        lab_walk_l0[index] = 0;
    }
}

static uint64_t vpn_of(uint64_t va, uint64_t level)
{
    return (va >> (LAB_PAGE_SHIFT + level * 9)) & UINT64_C(0x1ff);
}

static int test_address_geometry(void)
{
    const uint64_t va = UINT64_C(0x0000000123456789);
    uint64_t expected_satp;

    expected_satp = (UINT64_C(LAB_SATP_MODE_SV39) << LAB_SATP_MODE_SHIFT) |
                    (UINT64_C(7) << LAB_SATP_ASID_SHIFT) |
                    ((uintptr_t)lab_walk_root >> LAB_PAGE_SHIFT);
    return lab_sv39_is_canonical(UINT64_C(0x0000003fffffffff)) == 1 &&
           lab_sv39_is_canonical(UINT64_C(0xffffffc000000000)) == 1 &&
           lab_sv39_is_canonical(UINT64_C(0x0000004000000000)) == 0 &&
           lab_sv39_vpn(va, 0) == vpn_of(va, 0) &&
           lab_sv39_vpn(va, 1) == vpn_of(va, 1) &&
           lab_sv39_vpn(va, 2) == vpn_of(va, 2) &&
           lab_sv39_vpn(va, 3) == UINT64_MAX &&
           lab_make_satp(lab_walk_root, 7) == expected_satp &&
           lab_make_satp((uint64_t *)((uintptr_t)lab_walk_root + 8), 7) == 0 &&
           lab_make_satp(lab_walk_root, UINT64_C(0x10000)) == 0;
}

static int test_pte_construction(void)
{
    const uint64_t pa = UINT64_C(0x80234000);
    const uint64_t flags = LAB_PTE_R | LAB_PTE_W | LAB_PTE_A | LAB_PTE_D;
    const uint64_t expected_table = ((pa >> LAB_PAGE_SHIFT) << 10) |
                                    LAB_PTE_V;
    const uint64_t expected_leaf = expected_table | flags;

    return lab_make_table_pte(pa) == expected_table &&
           lab_make_table_pte(pa + 1) == 0 &&
           lab_make_leaf_pte(pa, flags) == expected_leaf &&
           lab_make_leaf_pte(pa, LAB_PTE_W | LAB_PTE_A) == 0 &&
           lab_make_leaf_pte(pa, LAB_PTE_A | LAB_PTE_D) == 0 &&
           lab_make_leaf_pte(pa + 8, flags) == 0 &&
           lab_make_leaf_pte(pa, UINT64_C(0x100)) == 0;
}

static int test_software_walk(void)
{
    const uint64_t va = UINT64_C(0x0000000123456788);
    const uint64_t page_pa = (uintptr_t)lab_test_page_a;
    const uint64_t leaf_flags = LAB_PTE_R | LAB_PTE_A;
    uint64_t output = 0;
    uint64_t l1_index;
    int result;

    zero_walk_tables();
    lab_walk_root[vpn_of(va, 2)] =
        lab_infra_table_pte((uintptr_t)lab_walk_l1);
    lab_walk_l1[vpn_of(va, 1)] =
        lab_infra_table_pte((uintptr_t)lab_walk_l0);
    lab_walk_l0[vpn_of(va, 0)] =
        lab_infra_leaf_pte(page_pa, leaf_flags);

    result = lab_sv39_translate(lab_walk_root, va, LAB_ACCESS_READ, &output);
    if (result != LAB_WALK_OK ||
        output != page_pa + (va & (LAB_PAGE_SIZE - 1))) {
        return 0;
    }
    if (lab_sv39_translate(lab_walk_root, va, LAB_ACCESS_WRITE, &output) !=
        LAB_WALK_ERR_PERMISSION) {
        return 0;
    }

    lab_walk_l0[vpn_of(va, 0)] = lab_infra_leaf_pte(
        page_pa, LAB_PTE_X | LAB_PTE_A);
    if (lab_sv39_translate(lab_walk_root, va, LAB_ACCESS_READ, &output) !=
            LAB_WALK_ERR_PERMISSION ||
        lab_sv39_translate(lab_walk_root, va,
                           LAB_ACCESS_READ | LAB_ACCESS_MXR, &output) !=
            LAB_WALK_OK) {
        return 0;
    }

    l1_index = vpn_of(va, 1);
    lab_walk_l1[l1_index] = lab_infra_leaf_pte(
        UINT64_C(0x80400000), LAB_PTE_R | LAB_PTE_A);
    if (lab_sv39_translate(lab_walk_root, va, LAB_ACCESS_READ, &output) !=
            LAB_WALK_OK ||
        output != UINT64_C(0x80400000) + (va & UINT64_C(0x1fffff))) {
        return 0;
    }

    lab_walk_l1[l1_index] = lab_infra_leaf_pte(
        UINT64_C(0x80401000), LAB_PTE_R | LAB_PTE_A);
    return lab_sv39_translate(lab_walk_root, va, LAB_ACCESS_READ, &output) ==
           LAB_WALK_ERR_SUPERPAGE;
}

static int test_mapping_contract(void)
{
    const uint64_t flags = LAB_PTE_R | LAB_PTE_W | LAB_PTE_A | LAB_PTE_D;
    const uint64_t expected = lab_infra_leaf_pte(
        (uintptr_t)lab_test_page_a, flags);

    zero_walk_tables();
    lab_walk_root[1] = lab_infra_table_pte((uintptr_t)lab_walk_l1);
    lab_walk_l1[0] = lab_infra_table_pte((uintptr_t)lab_walk_l0);
    if (!lab_install_4k(lab_walk_root, LAB_TEST_VA,
                        (uintptr_t)lab_test_page_a, flags) ||
        lab_walk_l0[0] != expected) {
        return 0;
    }
    if (!lab_unmap_4k(lab_walk_root, LAB_TEST_VA) ||
        lab_walk_l0[0] != 0) {
        return 0;
    }
    return lab_install_4k(lab_walk_root, LAB_TEST_VA + 1,
                          (uintptr_t)lab_test_page_a, flags) == 0 &&
           lab_install_4k(lab_walk_root, LAB_TEST_VA,
                          (uintptr_t)lab_test_page_a + 1, flags) == 0;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("Sv39 canonical/VPN/satp geometry",
                           test_address_geometry());
    failures += lab_report("table and leaf PTE construction",
                           test_pte_construction());
    failures += lab_report("software walk permissions and superpages",
                           test_software_walk());
    failures += lab_report("4 KiB install/unmap contract",
                           test_mapping_contract());
    return failures;
}

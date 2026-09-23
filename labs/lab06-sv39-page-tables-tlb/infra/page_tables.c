#include "lab_api.h"

uint64_t lab_mmu_root[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));
uint64_t lab_mmu_l1[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));
uint64_t lab_mmu_l0[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));
uint64_t lab_walk_root[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));
uint64_t lab_walk_l1[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));
uint64_t lab_walk_l0[LAB_PT_ENTRIES]
    __attribute__((section(".page_tables"), aligned(LAB_PAGE_SIZE)));

uint8_t lab_test_page_a[LAB_PAGE_SIZE]
    __attribute__((section(".test_pages"), aligned(LAB_PAGE_SIZE)));
uint8_t lab_test_page_b[LAB_PAGE_SIZE]
    __attribute__((section(".test_pages"), aligned(LAB_PAGE_SIZE)));

static void zero_page(uint64_t *page)
{
    uint64_t index;
    for (index = 0; index < LAB_PT_ENTRIES; ++index) {
        page[index] = 0;
    }
}

uint64_t lab_infra_table_pte(uint64_t pa)
{
    return ((pa >> LAB_PAGE_SHIFT) << 10) | LAB_PTE_V;
}

uint64_t lab_infra_leaf_pte(uint64_t pa, uint64_t flags)
{
    return ((pa >> LAB_PAGE_SHIFT) << 10) | flags | LAB_PTE_V;
}

uint64_t lab_infra_satp(uint64_t *root, uint64_t asid)
{
    return (UINT64_C(LAB_SATP_MODE_SV39) << LAB_SATP_MODE_SHIFT) |
           ((asid & UINT64_C(0xffff)) << LAB_SATP_ASID_SHIFT) |
           ((uintptr_t)root >> LAB_PAGE_SHIFT);
}

void lab_prepare_page_tables(int install_smoke_leaf)
{
    uint64_t ram_flags = LAB_PTE_R | LAB_PTE_W | LAB_PTE_X |
                         LAB_PTE_G | LAB_PTE_A | LAB_PTE_D;
    uint64_t io_flags = LAB_PTE_R | LAB_PTE_W | LAB_PTE_G |
                        LAB_PTE_A | LAB_PTE_D;

    zero_page(lab_mmu_root);
    zero_page(lab_mmu_l1);
    zero_page(lab_mmu_l0);

    lab_mmu_root[0] = lab_infra_leaf_pte(0, io_flags);
    lab_mmu_root[1] = lab_infra_table_pte((uintptr_t)lab_mmu_l1);
    lab_mmu_root[2] = lab_infra_leaf_pte(UINT64_C(0x80000000), ram_flags);
    lab_mmu_l1[0] = lab_infra_table_pte((uintptr_t)lab_mmu_l0);

    *(uint64_t *)(void *)lab_test_page_a = UINT64_C(0x1111222233334444);
    *(uint64_t *)(void *)lab_test_page_b = UINT64_C(0xaaaabbbbccccdddd);
    if (install_smoke_leaf) {
        lab_mmu_l0[0] = lab_infra_leaf_pte(
            (uintptr_t)lab_test_page_a,
            LAB_PTE_R | LAB_PTE_W | LAB_PTE_A | LAB_PTE_D);
    }
}

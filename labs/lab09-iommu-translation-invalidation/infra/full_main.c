#include "lab_api.h"

static struct lab_io_page_table full_stage1
    __attribute__((section(".io_tables"), aligned(LAB_PAGE_SIZE)));
static struct lab_io_page_table full_stage2
    __attribute__((section(".io_tables"), aligned(LAB_PAGE_SIZE)));

static int test_map_translate_unmap_fault(void)
{
    const uint64_t iova_page = UINT64_C(0x70001000);
    const uint64_t gpa_page = UINT64_C(0x71002000);
    const uint64_t spa_page = UINT64_C(0x82003000);
    const uint64_t offset = UINT64_C(0x2a0);
    struct lab_device_context context;
    struct lab_iommu_domain domain;
    struct lab_translation result;
    uint64_t reclaimable = 0;
    uint64_t stage2_pte;

    lab_memzero(&full_stage1, sizeof(full_stage1));
    lab_memzero(&full_stage2, sizeof(full_stage2));
    lab_memzero(&context, sizeof(context));
    lab_memzero(&domain, sizeof(domain));
    full_stage1.base_vpn = UINT64_C(0x70000000) >> LAB_PAGE_SHIFT;
    full_stage2.base_vpn = UINT64_C(0x71000000) >> LAB_PAGE_SHIFT;
    domain.domain_id = 21;
    domain.table = &full_stage1;
    domain.state = LAB_DOMAIN_READY;
    lab_iommu_queue_reset(1);

    if (!lab_iommu_map(&domain, iova_page, gpa_page,
                       LAB_IO_PERM_R | LAB_IO_PERM_W)) {
        return 0;
    }
    stage2_pte = lab_iopte_make(spa_page,
                                LAB_IO_PERM_R | LAB_IO_PERM_W);
    if (stage2_pte == UINT64_MAX) {
        return 0;
    }
    full_stage2.entries[2] = stage2_pte;
    context.device_id = 0x51;
    context.process_id = 4;
    context.flags = LAB_CONTEXT_STAGE1_ENABLE |
                    LAB_CONTEXT_STAGE2_ENABLE;
    context.stage1 = &full_stage1;
    context.stage2 = &full_stage2;

    if (!lab_iommu_translate(&context, 1, 0x51, 4,
                             iova_page + offset, LAB_IO_PERM_W, &result) ||
        result.gpa != gpa_page + offset ||
        result.spa != spa_page + offset) {
        return 0;
    }
    if (!lab_iommu_unmap(&domain, iova_page, &reclaimable) ||
        reclaimable != gpa_page || lab_iommu_trace_count != 2) {
        return 0;
    }
    lab_memzero(&result, sizeof(result));
    return !lab_iommu_translate(&context, 1, 0x51, 4,
                                iova_page + offset, LAB_IO_PERM_R,
                                &result) &&
           result.fault_cause == LAB_FAULT_INVALID_PTE &&
           result.fault_stage == LAB_FAULT_STAGE_1;
}

void lab_full_main(uint64_t hartid)
{
    unsigned int failures = 0;
    (void)hartid;

    lab_puts("LAB09 IOMMU translation and invalidation tests\n");
    failures += lab_report("map, invalidate, translate, unmap, and fault",
                           test_map_translate_unmap_fault());
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB09 PASS\n" : "LAB09 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

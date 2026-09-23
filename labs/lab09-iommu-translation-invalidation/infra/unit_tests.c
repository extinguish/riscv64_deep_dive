#include "lab_api.h"

_Static_assert(sizeof(struct lab_io_page_table) == LAB_IO_PT_SIZE,
               "assembly IO page-table layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_io_page_table, entries) ==
                   LAB_IO_PT_ENTRIES_OFFSET,
               "assembly IO page-table offsets mismatch");
_Static_assert(sizeof(struct lab_device_context) == LAB_CONTEXT_SIZE,
               "assembly device-context layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_device_context, stage2) ==
                   LAB_CONTEXT_STAGE2,
               "assembly device-context offsets mismatch");
_Static_assert(sizeof(struct lab_translation) == LAB_RESULT_SIZE,
               "assembly translation-result layout mismatch");
_Static_assert(sizeof(struct lab_iommu_domain) == LAB_DOMAIN_SIZE,
               "assembly domain layout mismatch");
_Static_assert(sizeof(struct lab_assignment) == LAB_ASSIGN_SIZE,
               "assembly assignment layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_assignment, msi_limit) ==
                   LAB_ASSIGN_MSI_LIMIT,
               "assembly assignment offsets mismatch");

static struct lab_io_page_table translation_stage1
    __attribute__((section(".io_tables"), aligned(LAB_PAGE_SIZE)));
static struct lab_io_page_table translation_stage2
    __attribute__((section(".io_tables"), aligned(LAB_PAGE_SIZE)));
static struct lab_io_page_table mapping_table
    __attribute__((section(".io_tables"), aligned(LAB_PAGE_SIZE)));

static uint64_t infra_pte(uint64_t physical_page, uint64_t permissions)
{
    uint64_t pte = physical_page | LAB_IO_PTE_V;

    if ((permissions & LAB_IO_PERM_R) != 0) {
        pte |= LAB_IO_PTE_R;
    }
    if ((permissions & LAB_IO_PERM_W) != 0) {
        pte |= LAB_IO_PTE_W;
    }
    return pte;
}

static int test_iopte_encoding(void)
{
    const uint64_t page = UINT64_C(0x0000000012345000);
    uint64_t decoded_page = 0;
    uint64_t permissions = 0;
    uint64_t pte = page | LAB_IO_PTE_V | LAB_IO_PTE_R | LAB_IO_PTE_W;

    if (lab_iopte_make(page, LAB_IO_PERM_R | LAB_IO_PERM_W) != pte ||
        !lab_iopte_decode(pte, &decoded_page, &permissions) ||
        decoded_page != page ||
        permissions != (LAB_IO_PERM_R | LAB_IO_PERM_W)) {
        return 0;
    }
    return lab_iopte_make(page + 1, LAB_IO_PERM_R) == UINT64_MAX &&
           lab_iopte_make(page, LAB_IO_PERM_W) == UINT64_MAX &&
           lab_iopte_make(page, 0) == UINT64_MAX &&
           lab_iopte_make(page, 4) == UINT64_MAX &&
           lab_iopte_make(UINT64_C(0x0010000000000000),
                          LAB_IO_PERM_R) == UINT64_MAX &&
           !lab_iopte_decode(0, &decoded_page, &permissions) &&
           !lab_iopte_decode(page | LAB_IO_PTE_V | LAB_IO_PTE_W,
                             &decoded_page, &permissions) &&
           !lab_iopte_decode(page | UINT64_C(0x9),
                             &decoded_page, &permissions) &&
           !lab_iopte_decode(pte, 0, &permissions) &&
           !lab_iopte_decode(pte, &decoded_page, 0);
}

static int test_context_translation(void)
{
    const uint64_t iova_page = UINT64_C(0x40002000);
    const uint64_t gpa_page = UINT64_C(0x50003000);
    const uint64_t spa_page = UINT64_C(0x80004000);
    const uint64_t offset = UINT64_C(0x123);
    struct lab_device_context context;
    struct lab_translation result;

    lab_memzero(&translation_stage1, sizeof(translation_stage1));
    lab_memzero(&translation_stage2, sizeof(translation_stage2));
    lab_memzero(&context, sizeof(context));
    translation_stage1.base_vpn = UINT64_C(0x40000000) >> LAB_PAGE_SHIFT;
    translation_stage2.base_vpn = UINT64_C(0x50000000) >> LAB_PAGE_SHIFT;
    translation_stage1.entries[2] =
        infra_pte(gpa_page, LAB_IO_PERM_R | LAB_IO_PERM_W);
    translation_stage2.entries[3] =
        infra_pte(spa_page, LAB_IO_PERM_R);
    context.device_id = 0x31;
    context.process_id = 7;
    context.flags = LAB_CONTEXT_STAGE1_ENABLE |
                    LAB_CONTEXT_STAGE2_ENABLE;
    context.stage1 = &translation_stage1;
    context.stage2 = &translation_stage2;

    lab_memzero(&result, sizeof(result));
    if (!lab_iommu_translate(&context, 1, 0x31, 7,
                             iova_page + offset, LAB_IO_PERM_R, &result) ||
        result.gpa != gpa_page + offset ||
        result.spa != spa_page + offset ||
        result.fault_cause != LAB_FAULT_NONE ||
        result.context_index != 0) {
        return 0;
    }

    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x31, 7,
                            iova_page + offset, LAB_IO_PERM_W, &result) ||
        result.fault_cause != LAB_FAULT_PERMISSION ||
        result.fault_stage != LAB_FAULT_STAGE_2 ||
        result.fault_address != gpa_page + offset ||
        result.gpa != gpa_page + offset) {
        return 0;
    }

    translation_stage2.entries[3] = 0;
    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x31, 7,
                            iova_page + offset, LAB_IO_PERM_R, &result) ||
        result.fault_cause != LAB_FAULT_INVALID_PTE ||
        result.fault_stage != LAB_FAULT_STAGE_2 ||
        result.fault_address != gpa_page + offset) {
        return 0;
    }
    translation_stage2.entries[3] =
        infra_pte(spa_page, LAB_IO_PERM_R);
    translation_stage2.base_vpn =
        UINT64_C(0x50004000) >> LAB_PAGE_SHIFT;
    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x31, 7,
                            iova_page + offset, LAB_IO_PERM_R, &result) ||
        result.fault_cause != LAB_FAULT_ADDRESS ||
        result.fault_stage != LAB_FAULT_STAGE_2 ||
        result.fault_address != gpa_page + offset) {
        return 0;
    }
    translation_stage2.base_vpn =
        UINT64_C(0x50000000) >> LAB_PAGE_SHIFT;

    translation_stage1.entries[2] =
        infra_pte(gpa_page, LAB_IO_PERM_R);
    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x31, 7,
                            iova_page + offset, LAB_IO_PERM_W, &result) ||
        result.fault_cause != LAB_FAULT_PERMISSION ||
        result.fault_stage != LAB_FAULT_STAGE_1 ||
        result.fault_address != iova_page + offset) {
        return 0;
    }

    translation_stage1.entries[2] = 0;
    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x31, 7,
                            iova_page + offset, LAB_IO_PERM_R, &result) ||
        result.fault_cause != LAB_FAULT_INVALID_PTE ||
        result.fault_stage != LAB_FAULT_STAGE_1 ||
        result.fault_address != iova_page + offset) {
        return 0;
    }

    translation_stage1.entries[2] =
        infra_pte(gpa_page, LAB_IO_PERM_R | LAB_IO_PERM_W);
    context.flags = LAB_CONTEXT_STAGE1_ENABLE;
    context.stage2 = 0;
    if (!lab_iommu_translate(&context, 1, 0x31, 7,
                             iova_page + offset, LAB_IO_PERM_R, &result) ||
        result.gpa != gpa_page + offset ||
        result.spa != gpa_page + offset) {
        return 0;
    }

    context.flags = LAB_CONTEXT_STAGE1_ENABLE |
                    LAB_CONTEXT_STAGE2_ENABLE;
    context.stage2 = &translation_stage2;
    lab_memzero(&result, sizeof(result));
    if (lab_iommu_translate(&context, 1, 0x32, 7,
                            iova_page, LAB_IO_PERM_R, &result) ||
        result.fault_cause != LAB_FAULT_CONTEXT ||
        result.fault_stage != LAB_FAULT_STAGE_CONTEXT ||
        result.context_index != UINT64_MAX) {
        return 0;
    }

    lab_memzero(&result, sizeof(result));
    return !lab_iommu_translate(&context, 1, 0x31, 7,
                                UINT64_C(0x40040000), LAB_IO_PERM_R,
                                &result) &&
           result.fault_cause == LAB_FAULT_ADDRESS &&
           result.fault_stage == LAB_FAULT_STAGE_1;
}

static int command_is(uint64_t index, uint64_t domain_id,
                      uint64_t iova, uint64_t scope)
{
    return index < lab_iommu_trace_count &&
           lab_iommu_trace[index].ticket == index + 1 &&
           lab_iommu_trace[index].domain_id == domain_id &&
           lab_iommu_trace[index].iova == iova &&
           lab_iommu_trace[index].scope == scope;
}

static int test_map_unmap_and_quarantine(void)
{
    const uint64_t base = UINT64_C(0x60000000);
    const uint64_t page1 = UINT64_C(0x81000000);
    const uint64_t page2 = UINT64_C(0x81001000);
    struct lab_iommu_domain domain;
    uint64_t reclaimable = UINT64_MAX;

    lab_memzero(&mapping_table, sizeof(mapping_table));
    lab_memzero(&domain, sizeof(domain));
    mapping_table.base_vpn = base >> LAB_PAGE_SHIFT;
    domain.domain_id = 12;
    domain.table = &mapping_table;
    domain.state = LAB_DOMAIN_READY;
    lab_iommu_queue_reset(1);

    if (!lab_iommu_map(&domain, base + LAB_PAGE_SIZE, page1,
                       LAB_IO_PERM_R | LAB_IO_PERM_W) ||
        mapping_table.entries[1] !=
            infra_pte(page1, LAB_IO_PERM_R | LAB_IO_PERM_W) ||
        domain.last_ticket != 1 || domain.state != LAB_DOMAIN_READY ||
        !command_is(0, 12, base + LAB_PAGE_SIZE,
                    LAB_IOMMU_SCOPE_PAGE)) {
        return 0;
    }
    if (!lab_iommu_unmap(&domain, base + LAB_PAGE_SIZE, &reclaimable) ||
        reclaimable != page1 || mapping_table.entries[1] != 0 ||
        domain.last_ticket != 2 || domain.quarantined_page != 0 ||
        !command_is(1, 12, base + LAB_PAGE_SIZE,
                    LAB_IOMMU_SCOPE_PAGE)) {
        return 0;
    }
    if (lab_iommu_map(&domain, base + LAB_IO_PT_ENTRIES * LAB_PAGE_SIZE,
                      page1, LAB_IO_PERM_R) ||
        lab_iommu_trace_count != 2) {
        return 0;
    }

    if (!lab_iommu_map(&domain, base + 2 * LAB_PAGE_SIZE, page2,
                       LAB_IO_PERM_R)) {
        return 0;
    }
    lab_iommu_queue_reset(0);
    reclaimable = UINT64_MAX;
    if (lab_iommu_unmap(&domain, base + 2 * LAB_PAGE_SIZE,
                        &reclaimable) ||
        reclaimable != 0 || mapping_table.entries[2] != 0 ||
        domain.state != LAB_DOMAIN_QUARANTINED ||
        domain.last_ticket != 1 || domain.quarantined_page != page2 ||
        !command_is(0, 12, base + 2 * LAB_PAGE_SIZE,
                    LAB_IOMMU_SCOPE_PAGE)) {
        return 0;
    }
    return !lab_iommu_map(&domain, base + 3 * LAB_PAGE_SIZE, page1,
                          LAB_IO_PERM_R) &&
           lab_iommu_trace_count == 1;
}

static int test_assignment_policy(void)
{
    struct lab_assignment assignment;

    lab_memzero(&assignment, sizeof(assignment));
    assignment.device_id = 0x45;
    assignment.group_id = 3;
    assignment.vmid = 9;
    assignment.dma_base = UINT64_C(0x80000000);
    assignment.dma_limit = UINT64_C(0x80010000);
    assignment.msi_base = UINT64_C(0x28000000);
    assignment.msi_limit = UINT64_C(0x28001000);

    if (!lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_BIND) ||
        assignment.state != LAB_ASSIGN_BOUND ||
        lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                 LAB_REQUEST_DMA_READ,
                                 UINT64_C(0x80000000), 8) ||
        !lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_START) ||
        assignment.state != LAB_ASSIGN_RUNNING) {
        return 0;
    }
    if (!lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                  LAB_REQUEST_DMA_READ,
                                  UINT64_C(0x80000000), 8) ||
        !lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                  LAB_REQUEST_DMA_WRITE,
                                  UINT64_C(0x8000fff0), 16) ||
        !lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                  LAB_REQUEST_MSI,
                                  UINT64_C(0x28000004), 4)) {
        return 0;
    }
    if (lab_assignment_authorize(&assignment, 0x46, 3, 9,
                                 LAB_REQUEST_DMA_READ,
                                 UINT64_C(0x80000000), 8) ||
        lab_assignment_authorize(&assignment, 0x45, 4, 9,
                                 LAB_REQUEST_DMA_READ,
                                 UINT64_C(0x80000000), 8) ||
        lab_assignment_authorize(&assignment, 0x45, 3, 10,
                                 LAB_REQUEST_DMA_READ,
                                 UINT64_C(0x80000000), 8) ||
        lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                 LAB_REQUEST_DMA_WRITE,
                                 UINT64_C(0x8000fff8), 16) ||
        lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                 LAB_REQUEST_DMA_READ,
                                 UINT64_MAX - 3, 8) ||
        lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                 LAB_REQUEST_MSI,
                                 UINT64_C(0x28000002), 4) ||
        lab_assignment_authorize(&assignment, 0x45, 3, 9,
                                 LAB_REQUEST_MSI,
                                 UINT64_C(0x28000004), 8)) {
        return 0;
    }
    if (lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_DETACH) ||
        !lab_assignment_transition(&assignment,
                                   LAB_ASSIGN_EVENT_QUIESCE) ||
        !lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_START) ||
        !lab_assignment_transition(&assignment,
                                   LAB_ASSIGN_EVENT_QUIESCE) ||
        lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_DETACH) ||
        !lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_RESET) ||
        !lab_assignment_transition(&assignment, LAB_ASSIGN_EVENT_DETACH)) {
        return 0;
    }
    return assignment.state == LAB_ASSIGN_DETACHED &&
           assignment.device_id == 0 && assignment.group_id == 0 &&
           assignment.vmid == 0 && assignment.dma_base == 0 &&
           assignment.dma_limit == 0 && assignment.msi_base == 0 &&
           assignment.msi_limit == 0;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("IO PTE encoding and reserved-state checks",
                           test_iopte_encoding());
    failures += lab_report("device context and two-stage translation",
                           test_context_translation());
    failures += lab_report("IOTLB completion gates page reclamation",
                           test_map_unmap_and_quarantine());
    failures += lab_report("assignment identity, apertures, and lifecycle",
                           test_assignment_policy());
    return failures;
}

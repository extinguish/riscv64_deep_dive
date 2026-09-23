#include "lab_api.h"

static int test_command_queue_auto_completion(void)
{
    uint64_t ticket;

    lab_iommu_queue_reset(1);
    ticket = lab_iommu_submit_invalidation(7, UINT64_C(0x40002000),
                                           LAB_IOMMU_SCOPE_PAGE);
    return ticket == 1 && lab_iommu_trace_count == 1 &&
           lab_iommu_completion_ticket == 1 &&
           lab_iommu_trace[0].domain_id == 7 &&
           lab_iommu_trace[0].iova == UINT64_C(0x40002000) &&
           lab_iommu_trace[0].scope == LAB_IOMMU_SCOPE_PAGE &&
           lab_iommu_wait_completion(ticket, LAB_IOMMU_WAIT_LIMIT);
}

static int test_timeout_and_manual_completion(void)
{
    uint64_t ticket;

    lab_iommu_queue_reset(0);
    ticket = lab_iommu_submit_invalidation(9, 0,
                                           LAB_IOMMU_SCOPE_DOMAIN);
    if (ticket != 1 || lab_iommu_wait_completion(ticket, 4) ||
        !lab_iommu_complete_through(ticket)) {
        return 0;
    }
    return lab_iommu_wait_completion(ticket, 4) &&
           !lab_iommu_submit_invalidation(9, 0, 0);
}

void lab_smoke_main(uint64_t hartid)
{
    unsigned int failures = 0;
    (void)hartid;

    lab_puts("LAB09 infrastructure smoke tests\n");
    failures += lab_report("invalidation queue and auto completion",
                           test_command_queue_auto_completion());
    failures += lab_report("bounded timeout and manual completion",
                           test_timeout_and_manual_completion());
    lab_puts(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

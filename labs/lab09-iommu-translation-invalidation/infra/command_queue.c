#include "lab_api.h"

volatile struct lab_iommu_command_event
    lab_iommu_trace[LAB_IOMMU_TRACE_MAX];
volatile uint64_t lab_iommu_trace_count;
volatile uint64_t lab_iommu_completion_ticket;
volatile uint64_t lab_iommu_auto_complete;

void lab_iommu_queue_reset(uint64_t auto_complete)
{
    uint64_t index;

    lab_iommu_trace_count = 0;
    lab_iommu_completion_ticket = 0;
    lab_iommu_auto_complete = auto_complete != 0;
    for (index = 0; index < LAB_IOMMU_TRACE_MAX; ++index) {
        lab_iommu_trace[index].ticket = 0;
        lab_iommu_trace[index].domain_id = 0;
        lab_iommu_trace[index].iova = 0;
        lab_iommu_trace[index].scope = 0;
    }
}

uint64_t __attribute__((section(".text.flow")))
lab_iommu_submit_invalidation(uint64_t domain_id, uint64_t iova,
                              uint64_t scope)
{
    uint64_t index = lab_iommu_trace_count;
    uint64_t ticket;

    if (index >= LAB_IOMMU_TRACE_MAX ||
        (scope != LAB_IOMMU_SCOPE_PAGE &&
         scope != LAB_IOMMU_SCOPE_DOMAIN)) {
        return 0;
    }
    ticket = index + 1;
    lab_iommu_trace[index].ticket = ticket;
    lab_iommu_trace[index].domain_id = domain_id;
    lab_iommu_trace[index].iova = iova;
    lab_iommu_trace[index].scope = scope;
    __asm__ volatile ("fence w,w" ::: "memory");
    lab_iommu_trace_count = ticket;
    if (lab_iommu_auto_complete != 0) {
        lab_iommu_completion_ticket = ticket;
    }
    return ticket;
}

int __attribute__((section(".text.flow")))
lab_iommu_wait_completion(uint64_t ticket, uint64_t spin_limit)
{
    uint64_t spin;

    if (ticket == 0 || spin_limit == 0 ||
        ticket > lab_iommu_trace_count) {
        return 0;
    }
    for (spin = 0; spin < spin_limit; ++spin) {
        if (lab_iommu_completion_ticket >= ticket) {
            __asm__ volatile ("fence r,rw" ::: "memory");
            return 1;
        }
    }
    return 0;
}

int __attribute__((section(".text.flow")))
lab_iommu_complete_through(uint64_t ticket)
{
    if (ticket == 0 || ticket > lab_iommu_trace_count) {
        return 0;
    }
    __asm__ volatile ("fence rw,w" ::: "memory");
    if (ticket > lab_iommu_completion_ticket) {
        lab_iommu_completion_ticket = ticket;
    }
    return 1;
}

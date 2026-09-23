#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_layout.h"

struct lab_io_page_table {
    uint64_t base_vpn;
    volatile uint64_t entries[LAB_IO_PT_ENTRIES];
};

struct lab_device_context {
    uint64_t device_id;
    uint64_t process_id;
    uint64_t flags;
    uint64_t reserved;
    struct lab_io_page_table *stage1;
    struct lab_io_page_table *stage2;
};

struct lab_translation {
    uint64_t gpa;
    uint64_t spa;
    uint64_t fault_address;
    uint64_t fault_cause;
    uint64_t fault_stage;
    uint64_t context_index;
};

struct lab_iommu_domain {
    uint64_t domain_id;
    struct lab_io_page_table *table;
    volatile uint64_t state;
    volatile uint64_t last_ticket;
    volatile uint64_t quarantined_page;
};

struct lab_assignment {
    uint64_t device_id;
    uint64_t group_id;
    uint64_t vmid;
    uint64_t state;
    uint64_t dma_base;
    uint64_t dma_limit;
    uint64_t msi_base;
    uint64_t msi_limit;
} __attribute__((aligned(64)));

struct lab_iommu_command_event {
    uint64_t ticket;
    uint64_t domain_id;
    uint64_t iova;
    uint64_t scope;
};

uint64_t lab_iopte_make(uint64_t physical_page, uint64_t permissions);
int lab_iopte_decode(uint64_t pte, uint64_t *physical_page,
                     uint64_t *permissions);
int lab_iommu_translate(const struct lab_device_context *contexts,
                        uint64_t context_count, uint64_t device_id,
                        uint64_t process_id, uint64_t iova,
                        uint64_t access,
                        struct lab_translation *result);
int lab_iommu_map(struct lab_iommu_domain *domain, uint64_t iova,
                  uint64_t physical_page, uint64_t permissions);
int lab_iommu_unmap(struct lab_iommu_domain *domain, uint64_t iova,
                    uint64_t *reclaimable_page);
int lab_assignment_transition(struct lab_assignment *assignment,
                              uint64_t event);
int lab_assignment_authorize(const struct lab_assignment *assignment,
                             uint64_t device_id, uint64_t group_id,
                             uint64_t vmid, uint64_t request,
                             uint64_t address, uint64_t size);

void lab_hart_entry(uint64_t hartid) __attribute__((noreturn));
unsigned int lab_run_unit_tests(void);

void *lab_memzero(void *destination, uint64_t size);
void lab_iommu_queue_reset(uint64_t auto_complete);
uint64_t lab_iommu_submit_invalidation(uint64_t domain_id,
                                       uint64_t iova, uint64_t scope);
int lab_iommu_wait_completion(uint64_t ticket, uint64_t spin_limit);
int lab_iommu_complete_through(uint64_t ticket);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);

extern volatile struct lab_iommu_command_event
    lab_iommu_trace[LAB_IOMMU_TRACE_MAX];
extern volatile uint64_t lab_iommu_trace_count;
extern volatile uint64_t lab_iommu_completion_ticket;
extern volatile uint64_t lab_iommu_auto_complete;

extern unsigned char __global_pointer$[];
extern unsigned char __stack_top[];
extern unsigned char __bss_start[];
extern unsigned char __bss_end[];
extern unsigned char __io_tables_start[];
extern unsigned char __io_tables_end[];

#endif

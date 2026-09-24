#include "lab_api.h"

static int test_sparse_hart_mapping(void)
{
    static const uint64_t sparse[] = { 2, 9, 42, UINT64_MAX - 1 };
    return lab_hartid_to_cpu(2, sparse, 4) == 0 &&
           lab_hartid_to_cpu(42, sparse, 4) == 2 &&
           lab_hartid_to_cpu(UINT64_MAX - 1, sparse, 4) == 3 &&
           lab_hartid_to_cpu(0, sparse, 4) == UINT64_MAX &&
           lab_hartid_to_cpu(2, 0, 4) == UINT64_MAX &&
           lab_hartid_to_cpu(2, 0, 0) == UINT64_MAX;
}

static int test_hart_mask_windows(void)
{
    static const uint64_t sparse[] = { 2, 9, 42, 70 };
    uint64_t low = (UINT64_C(1) << 2) | (UINT64_C(1) << 9) |
                   (UINT64_C(1) << 42);
    return lab_hart_mask_chunk(sparse, 4, 0, 64) == low &&
           lab_hart_mask_chunk(sparse, 4, 64, 64) ==
               (UINT64_C(1) << 6) &&
           lab_hart_mask_chunk(0, 0, 128, 64) == 0 &&
           lab_hart_mask_chunk(0, 1, 0, 64) == UINT64_MAX &&
           lab_hart_mask_chunk(sparse, 4, 0, 0) == UINT64_MAX &&
           lab_hart_mask_chunk(sparse, 4, 0, 65) == UINT64_MAX;
}

static int test_lifecycle(void)
{
    return lab_cpu_transition(LAB_CPU_OFFLINE, LAB_CPU_EVENT_START) ==
               LAB_CPU_START_PENDING &&
           lab_cpu_transition(LAB_CPU_START_PENDING,
                              LAB_CPU_EVENT_START_OK) == LAB_CPU_ONLINE &&
           lab_cpu_transition(LAB_CPU_START_PENDING,
                              LAB_CPU_EVENT_START_FAIL) == LAB_CPU_FAILED &&
           lab_cpu_transition(LAB_CPU_ONLINE, LAB_CPU_EVENT_STOP) ==
               LAB_CPU_STOP_PENDING &&
           lab_cpu_transition(LAB_CPU_STOP_PENDING,
                              LAB_CPU_EVENT_STOP_OK) == LAB_CPU_OFFLINE &&
           lab_cpu_transition(LAB_CPU_STOP_PENDING,
                              LAB_CPU_EVENT_STOP_FAIL) == LAB_CPU_ONLINE &&
           lab_cpu_transition(LAB_CPU_ONLINE, LAB_CPU_EVENT_START) ==
               UINT64_MAX &&
           lab_cpu_transition(LAB_CPU_STATE_COUNT, LAB_CPU_EVENT_START) ==
               UINT64_MAX &&
           lab_cpu_transition(LAB_CPU_OFFLINE, LAB_CPU_EVENT_COUNT + 1) ==
               UINT64_MAX;
}

static int test_ipi_mailbox(void)
{
    volatile uint64_t mailbox = LAB_IPI_RESCHEDULE;
    volatile uint64_t ready = LAB_IPI_WORK_MASK;
    uint64_t old;
    uint64_t reasons;

    old = lab_ipi_publish(&mailbox, LAB_IPI_CALL_FUNCTION);
    if (old != LAB_IPI_RESCHEDULE || mailbox != LAB_IPI_WORK_MASK) return 0;
    if (lab_wait_for_bits(&ready, LAB_IPI_WORK_MASK) != LAB_IPI_WORK_MASK)
        return 0;
    reasons = lab_ipi_consume(&mailbox);
    return reasons == LAB_IPI_WORK_MASK && mailbox == 0 &&
           lab_ipi_consume(&mailbox) == 0 &&
           lab_ipi_publish(&mailbox, 0) == UINT64_MAX && mailbox == 0 &&
           lab_ipi_publish(&mailbox, 0x10) == UINT64_MAX && mailbox == 0 &&
           lab_ipi_publish(0, LAB_IPI_TLB) == UINT64_MAX &&
           lab_ipi_consume(0) == UINT64_MAX &&
           lab_wait_for_bits(0, 1) == 0 &&
           lab_wait_for_bits(&ready, 0) == 0;
}

static int test_timer_policy(void)
{
    uint64_t storage[2] = { 0, 0 };
    volatile uint64_t *misaligned =
        (volatile uint64_t *)((uintptr_t)&storage[0] + 1);
    return lab_timer_select(100, 150, 120) == 120 &&
           lab_timer_select(100, 80, 200) == 100 &&
           lab_timer_select(100, UINT64_MAX, 140) == 140 &&
           lab_timer_select(100, UINT64_MAX, UINT64_MAX) == UINT64_MAX &&
           lab_timer_program(&storage[0], UINT64_C(0x123456789abcdef0)) &&
           storage[0] == UINT64_C(0x123456789abcdef0) &&
           !lab_timer_program(0, 10) &&
           !lab_timer_program(misaligned, 10);
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;
    failures += lab_report("sparse hartid to logical CPU mapping",
                           test_sparse_hart_mapping());
    failures += lab_report("SBI hart-mask window construction",
                           test_hart_mask_windows());
    failures += lab_report("HSM-style lifecycle transitions", test_lifecycle());
    failures += lab_report("coalesced IPI mailbox and WFI contract",
                           test_ipi_mailbox());
    failures += lab_report("absolute deadline selection and programming",
                           test_timer_policy());
    return failures;
}

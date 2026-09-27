#include "lab_api.h"

_Static_assert(sizeof(struct lab_plic_context_entry) == 24,
               "assembly PLIC context entry size mismatch");
_Static_assert(__builtin_offsetof(struct lab_plic_context_entry,
                                  machine_context) == 8,
               "assembly machine-context offset mismatch");
_Static_assert(__builtin_offsetof(struct lab_plic_context_entry,
                                  supervisor_context) == 16,
               "assembly supervisor-context offset mismatch");

static int test_context_discovery(void)
{
    static const struct lab_plic_context_entry table[] = {
        { 2, 0, 1 },
        { 9, 4, UINT64_MAX },
        { 42, 8, 9 },
    };
    return lab_plic_context_lookup(2, LAB_PRIV_M, table, 3) == 0 &&
           lab_plic_context_lookup(2, LAB_PRIV_S, table, 3) == 1 &&
           lab_plic_context_lookup(42, LAB_PRIV_S, table, 3) == 9 &&
           lab_plic_context_lookup(9, LAB_PRIV_S, table, 3) == UINT64_MAX &&
           lab_plic_context_lookup(0, LAB_PRIV_M, table, 3) == UINT64_MAX &&
           lab_plic_context_lookup(2, 0, table, 3) == UINT64_MAX &&
           lab_plic_context_lookup(2, LAB_PRIV_M, 0, 3) == UINT64_MAX;
}

static int test_plic_arbitration(void)
{
    static const uint32_t priority[] = { 0, 3, 5, 5, 1 };
    uint64_t pending = (UINT64_C(1) << 1) | (UINT64_C(1) << 2) |
                       (UINT64_C(1) << 3) | (UINT64_C(1) << 4);
    return lab_plic_select(priority, pending, pending, 2, 4) == 2 &&
           lab_plic_select(priority, pending,
                           pending & ~(UINT64_C(1) << 2), 2, 4) == 3 &&
           lab_plic_select(priority, pending, pending, 5, 4) == 0 &&
           lab_plic_select(priority, pending, UINT64_C(1) << 4, 0, 4) == 4 &&
           lab_plic_select(priority, 0, pending, 0, 4) == 0 &&
           lab_plic_select(0, pending, pending, 0, 4) == UINT64_MAX &&
           lab_plic_select(priority, pending, pending, 0, 64) == UINT64_MAX;
}

static int test_claim_ack_complete(void)
{
    volatile uint32_t claim_complete = 7;
    volatile uint8_t device_ack = 0xff;
    uint64_t result = lab_plic_service(&claim_complete, &device_ack, 0, 15);
    if (result != 7 || device_ack != 0 || claim_complete != 7) return 0;
    claim_complete = 0;
    device_ack = 0xa5;
    if (lab_plic_service(&claim_complete, &device_ack, 0, 15) != 0 ||
        device_ack != 0xa5) return 0;
    claim_complete = 16;
    if (lab_plic_service(&claim_complete, &device_ack, 0, 15) != UINT64_MAX ||
        device_ack != 0xa5 || claim_complete != 16) return 0;
    return lab_plic_service(0, &device_ack, 0, 15) == UINT64_MAX &&
           lab_plic_service(&claim_complete, 0, 0, 15) == UINT64_MAX;
}

static int test_affinity_retarget(void)
{
    uint64_t old_enable = (UINT64_C(1) << 5) | (UINT64_C(1) << 9);
    uint64_t new_enable = UINT64_C(1) << 2;
    if (!lab_plic_retarget(&old_enable, &new_enable, 5, 0)) return 0;
    if ((old_enable & (UINT64_C(1) << 5)) != 0 ||
        (new_enable & (UINT64_C(1) << 5)) == 0) return 0;
    return !lab_plic_retarget(&old_enable, &new_enable, 9, 1) &&
           !lab_plic_retarget(&old_enable, &new_enable, 0, 0) &&
           !lab_plic_retarget(&old_enable, &old_enable, 9, 0) &&
           !lab_plic_retarget(0, &new_enable, 9, 0);
}

static int test_aplic_targets(void)
{
    uint64_t direct = (UINT64_C(3) << LAB_APLIC_HART_SHIFT) | 7;
    uint64_t msi = (UINT64_C(2) << LAB_APLIC_HART_SHIFT) |
                   (UINT64_C(3) << LAB_APLIC_GUEST_SHIFT) | 45;
    return lab_aplic_direct_target(3, 7, 4, 31) == direct &&
           lab_aplic_direct_target(4, 7, 4, 31) == UINT64_MAX &&
           lab_aplic_direct_target(3, 0, 4, 31) == UINT64_MAX &&
           lab_aplic_direct_target(3, 32, 4, 31) == UINT64_MAX &&
           lab_aplic_msi_target(2, 3, 45, 4, 3, 128) == msi &&
           (msi & (UINT64_C(1) << 11)) == 0 &&
           lab_aplic_msi_target(4, 0, 1, 4, 3, 128) == UINT64_MAX &&
           lab_aplic_msi_target(2, 4, 1, 4, 3, 128) == UINT64_MAX &&
           lab_aplic_msi_target(2, 0, 0, 4, 3, 128) == UINT64_MAX &&
           lab_aplic_msi_target(2, 0, 128, 4, 3, 128) == UINT64_MAX;
}

static int test_imsic_guest_allocation(void)
{
    volatile uint64_t bitmap = (UINT64_C(1) << 0) | (UINT64_C(1) << 2);
    uint64_t guest = lab_imsic_guest_allocate(&bitmap, 4, 3);
    if (guest != 3 || (bitmap & (UINT64_C(1) << 3)) == 0) return 0;
    guest = lab_imsic_guest_allocate(&bitmap, 4, 2);
    if (guest != 1 || (bitmap & (UINT64_C(1) << 1)) == 0) return 0;
    bitmap = 0x1f;
    return lab_imsic_guest_allocate(&bitmap, 4, 0) == UINT64_MAX &&
           lab_imsic_guest_allocate(&bitmap, 0, 0) == UINT64_MAX &&
           lab_imsic_guest_allocate(&bitmap, 64, 0) == UINT64_MAX &&
           lab_imsic_guest_allocate(0, 4, 0) == UINT64_MAX;
}

static int test_indirect_window(void)
{
    volatile uint32_t selector = 7;
    volatile uint32_t data = 0x55;
    uint64_t result;
    result = lab_indirect_window_access(&selector, &data, 0x80, 0,
                                        LAB_INDIRECT_READ);
    if (result != 0x55 || selector != 7 || data != 0x55) return 0;
    result = lab_indirect_window_access(&selector, &data, 0x81, 0xaa,
                                        LAB_INDIRECT_WRITE);
    if (result != 0xaa || selector != 7 || data != 0xaa) return 0;
    return lab_indirect_window_access(&selector, &data, 0x82, 0, 2) ==
               UINT64_MAX &&
           lab_indirect_window_access(0, &data, 0x82, 0,
                                      LAB_INDIRECT_READ) == UINT64_MAX;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;
    failures += lab_report("firmware-described PLIC context lookup",
                           test_context_discovery());
    failures += lab_report("PLIC priority and threshold arbitration",
                           test_plic_arbitration());
    failures += lab_report("claim, device acknowledge, complete ordering",
                           test_claim_ack_complete());
    failures += lab_report("quiescent interrupt affinity migration",
                           test_affinity_retarget());
    failures += lab_report("APLIC direct and MSI target encoding",
                           test_aplic_targets());
    failures += lab_report("IMSIC guest-file allocation",
                           test_imsic_guest_allocation());
    failures += lab_report("serialized indirect CSR window",
                           test_indirect_window());
    return failures;
}

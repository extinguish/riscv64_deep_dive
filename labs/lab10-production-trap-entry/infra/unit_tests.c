#include "lab_api.h"

_Static_assert(sizeof(struct lab_pt_regs) == LAB_PT_SIZE,
               "assembly pt_regs size mismatch");
_Static_assert(__builtin_offsetof(struct lab_pt_regs, x[LAB_X_A0]) ==
                   LAB_PT_A0,
               "assembly GPR offsets mismatch");
_Static_assert(__builtin_offsetof(struct lab_pt_regs, sstatus) ==
                   LAB_PT_SSTATUS,
               "assembly CSR offsets mismatch");
_Static_assert(__builtin_offsetof(struct lab_pt_regs, reserved) ==
                   LAB_PT_RESERVED,
               "assembly pt_regs tail mismatch");
_Static_assert(sizeof(struct lab_cause_decode) == 16,
               "assembly cause-decode layout mismatch");

static uint64_t expected_trap_status(uint64_t status, uint64_t privilege)
{
    uint64_t result = status &
        ~(UINT64_C(LAB_SSTATUS_SIE) |
          UINT64_C(LAB_SSTATUS_SPIE) |
          UINT64_C(LAB_SSTATUS_SPP));

    if ((status & LAB_SSTATUS_SIE) != 0) {
        result |= LAB_SSTATUS_SPIE;
    }
    if (privilege == LAB_PRIV_S) {
        result |= LAB_SSTATUS_SPP;
    }
    return result;
}

static int test_status_state_machine(void)
{
    uint64_t status = UINT64_C(0x100000000) |
                      LAB_SSTATUS_SIE | LAB_SSTATUS_SPIE |
                      LAB_SSTATUS_SPP | LAB_SSTATUS_FS_MASK |
                      LAB_SSTATUS_VS_MASK;
    uint64_t trapped_u = lab_status_on_trap(status, LAB_PRIV_U);
    uint64_t trapped_s = lab_status_on_trap(status, LAB_PRIV_S);
    uint64_t sret_without_ie = lab_status_on_sret(
        status & ~(UINT64_C(LAB_SSTATUS_SIE) |
                   UINT64_C(LAB_SSTATUS_SPIE)));
    uint64_t sret_with_ie = lab_status_on_sret(
        status | LAB_SSTATUS_SPIE);

    return trapped_u == expected_trap_status(status, LAB_PRIV_U) &&
           trapped_s == expected_trap_status(status, LAB_PRIV_S) &&
           lab_status_on_trap(status, 2) == UINT64_MAX &&
           (sret_without_ie & LAB_SSTATUS_SIE) == 0 &&
           (sret_without_ie & LAB_SSTATUS_SPIE) != 0 &&
           (sret_without_ie & LAB_SSTATUS_SPP) == 0 &&
           (sret_with_ie & LAB_SSTATUS_SIE) != 0 &&
           (sret_with_ie & LAB_SSTATUS_SPIE) != 0 &&
           (sret_with_ie & LAB_SSTATUS_SPP) == 0;
}

static int test_user_status_sanitization(void)
{
    uint64_t input = UINT64_C(0x100000000) |
                     LAB_SSTATUS_SIE | LAB_SSTATUS_SPP |
                     LAB_SSTATUS_SUM | LAB_SSTATUS_MXR |
                     LAB_SSTATUS_FS_MASK | LAB_SSTATUS_VS_MASK;
    uint64_t output = lab_sanitize_user_sstatus(input);

    return (output & (LAB_SSTATUS_SIE | LAB_SSTATUS_SPP |
                      LAB_SSTATUS_SUM | LAB_SSTATUS_MXR)) == 0 &&
           (output & LAB_SSTATUS_SPIE) != 0 &&
           (output & LAB_SSTATUS_FS_MASK) == LAB_SSTATUS_FS_MASK &&
           (output & LAB_SSTATUS_VS_MASK) == LAB_SSTATUS_VS_MASK &&
           (output & UINT64_C(0x100000000)) != 0;
}

static int test_bounded_cause_decode(void)
{
    struct lab_cause_decode decoded;

    lab_memzero(&decoded, sizeof(decoded));
    if (!lab_decode_scause(3, 16, 16, &decoded) ||
        decoded.is_interrupt != 0 || decoded.cause != 3) {
        return 0;
    }
    if (!lab_decode_scause(UINT64_C(LAB_SCAUSE_INTERRUPT_BIT) | 9,
                           16, 16, &decoded) ||
        decoded.is_interrupt != 1 || decoded.cause != 9) {
        return 0;
    }
    return !lab_decode_scause(16, 16, 16, &decoded) &&
           !lab_decode_scause(UINT64_C(LAB_SCAUSE_INTERRUPT_BIT) | 16,
                              16, 16, &decoded) &&
           !lab_decode_scause(UINT64_C(LAB_SCAUSE_INTERRUPT_BIT) |
                              UINT64_C(0x100000), 16, 16, &decoded) &&
           !lab_decode_scause(3, 16, 16, 0);
}

static int test_instruction_length_and_epc(void)
{
    return lab_advance_epc(UINT64_C(0x1000), UINT64_C(0x9002)) ==
               UINT64_C(0x1002) &&
           lab_advance_epc(UINT64_C(0x1000), UINT64_C(0x0073)) ==
               UINT64_C(0x1004) &&
           lab_advance_epc(UINT64_C(0x1000), UINT64_C(0x001f)) ==
               UINT64_MAX &&
           lab_advance_epc(UINT64_C(0x1001), UINT64_C(0x9002)) ==
               UINT64_MAX &&
           lab_advance_epc(UINT64_MAX - 1, UINT64_C(0x9002)) ==
               UINT64_MAX;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("SIE/SPIE/SPP trap and sret transitions",
                           test_status_state_machine());
    failures += lab_report("safe user sstatus sanitization",
                           test_user_status_sanitization());
    failures += lab_report("bounded interrupt/exception cause decode",
                           test_bounded_cause_decode());
    failures += lab_report("RVC/32-bit instruction length and EPC",
                           test_instruction_length_and_epc());
    return failures;
}

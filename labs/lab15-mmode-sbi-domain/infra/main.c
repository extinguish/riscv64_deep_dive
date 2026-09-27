#include "lab_api.h"

uint64_t lab_firmware_secret = UINT64_C(0xfeedfacec0decafe);
static unsigned failures;
static unsigned pmp_violations;
static __attribute__((unused)) unsigned sbi_calls;

static __attribute__((unused)) unsigned run_units(void)
{
    unsigned failed = 0;
    lab_frame frame;
    volatile uint64_t *frame_words = (volatile uint64_t *)(void *)&frame;
    for (unsigned i = 0; i < sizeof(frame) / sizeof(uint64_t); i++)
        frame_words[i] = 0;
    uint64_t expected_pmp = (LAB_S_BASE >> 2) | ((UINT64_C(0x200000) >> 3) - 1);
    failed += lab_report("physical range validation",
        lab_domain_range(LAB_S_BASE, 8, LAB_S_BASE, LAB_S_END) == 1 &&
        lab_domain_range(LAB_S_END - 8, 8, LAB_S_BASE, LAB_S_END) == 1 &&
        lab_domain_range(LAB_S_BASE - 8, 8, LAB_S_BASE, LAB_S_END) == 0 &&
        lab_domain_range(LAB_S_END - 8, 16, LAB_S_BASE, LAB_S_END) == 0 &&
        lab_domain_range(UINT64_MAX - 7, 16, LAB_S_BASE, LAB_S_END) == 0 &&
        lab_domain_range(LAB_S_BASE + 1, 8, LAB_S_BASE, LAB_S_END) == 0 &&
        lab_domain_range(LAB_S_BASE, 0, LAB_S_BASE, LAB_S_END) == 0);
    failed += lab_report("PMP NAPOT encoding and invalid ranges",
        lab_pmp_napot(LAB_S_BASE, UINT64_C(0x200000)) == expected_pmp &&
        lab_pmp_napot(LAB_S_BASE, 8) == (LAB_S_BASE >> 2) &&
        lab_pmp_napot(LAB_S_BASE + 8, 16) == LAB_INVALID &&
        lab_pmp_napot(LAB_S_BASE, 12) == LAB_INVALID &&
        lab_pmp_napot(UINT64_MAX - 7, 16) == LAB_INVALID);

    frame.a7 = LAB_SBI_BASE;
    frame.a6 = 0;
    frame.a2 = UINT64_C(0xdeadbeef);
    frame.s0 = UINT64_C(0xcafebabe);
    lab_sbi_dispatch(&frame);
    failed += lab_report("SBI Base version and register preservation",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS &&
        frame.a1 == LAB_SBI_SPEC_V3 &&
        frame.a2 == UINT64_C(0xdeadbeef) &&
        frame.s0 == UINT64_C(0xcafebabe) &&
        frame.a6 == 0 && frame.a7 == LAB_SBI_BASE);
    frame.a7 = LAB_SBI_BASE; frame.a6 = 1;
    lab_sbi_dispatch(&frame);
    failed += lab_report("SBI Base implementation identity",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS &&
        frame.a1 == LAB_SBI_IMPL_ID);
    frame.a7 = LAB_SBI_BASE; frame.a6 = 2;
    lab_sbi_dispatch(&frame);
    failed += lab_report("SBI Base implementation version",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS &&
        frame.a1 == LAB_SBI_IMPL_VERSION);
    frame.a7 = LAB_SBI_BASE; frame.a6 = 3; frame.a0 = LAB_SBI_EXPERIMENTAL;
    lab_sbi_dispatch(&frame);
    failed += lab_report("SBI Base extension probe",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS && frame.a1 == 1);
    frame.a7 = LAB_SBI_BASE; frame.a6 = 3; frame.a0 = UINT64_C(0x42424242);
    lab_sbi_dispatch(&frame);
    failed += lab_report("unknown extension probe returns zero",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS && frame.a1 == 0);
    uint64_t csr_ids[3];
    __asm__ volatile ("csrr %0, mvendorid; csrr %1, marchid; csrr %2, mimpid"
                      : "=r"(csr_ids[0]), "=r"(csr_ids[1]), "=r"(csr_ids[2]));
    unsigned ids_ok = 1;
    for (unsigned i = 0; i < 3; i++) {
        frame.a7 = LAB_SBI_BASE; frame.a6 = 4 + i;
        lab_sbi_dispatch(&frame);
        if ((int64_t)frame.a0 != LAB_SBI_SUCCESS || frame.a1 != csr_ids[i])
            ids_ok = 0;
    }
    failed += lab_report("SBI Base machine ID FIDs 4-6", ids_ok);
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 0;
    frame.a0 = (uint64_t)(uintptr_t)&lab_s_word;
    lab_sbi_dispatch(&frame);
    failed += lab_report("validated S-owned physical buffer",
        (int64_t)frame.a0 == LAB_SBI_SUCCESS && frame.a1 == LAB_S_MAGIC);
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 0;
    frame.a0 = (uint64_t)(uintptr_t)&lab_firmware_secret;
    lab_sbi_dispatch(&frame);
    failed += lab_report("firmware address denied to SBI caller",
        (int64_t)frame.a0 == LAB_SBI_INVALID_ADDRESS && frame.a1 == 0);
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 0;
    frame.a0 = (uint64_t)(uintptr_t)&lab_s_word + 1;
    lab_sbi_dispatch(&frame);
    unsigned bad_unaligned = (int64_t)frame.a0 == LAB_SBI_INVALID_ADDRESS;
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 0;
    frame.a0 = LAB_S_END - 4;
    lab_sbi_dispatch(&frame);
    unsigned bad_edge = (int64_t)frame.a0 == LAB_SBI_INVALID_ADDRESS;
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 0;
    frame.a0 = UINT64_MAX - 7;
    lab_sbi_dispatch(&frame);
    failed += lab_report("misaligned, crossing and overflow buffers",
        bad_unaligned && bad_edge &&
        (int64_t)frame.a0 == LAB_SBI_INVALID_ADDRESS);
    frame.a7 = LAB_SBI_EXPERIMENTAL; frame.a6 = 99;
    lab_sbi_dispatch(&frame);
    failed += lab_report("unknown SBI function rejected",
        (int64_t)frame.a0 == LAB_SBI_NOT_SUPPORTED && frame.a1 == 0);
    return failed;
}

static __attribute__((unused)) void install_pmp(void)
{
    uint64_t deny_top = LAB_S_BASE >> 2;
    uint64_t allow = lab_pmp_napot(LAB_S_BASE, LAB_S_END - LAB_S_BASE);
    if (allow == LAB_INVALID) {
        lab_puts("invalid learner PMP encoding\n"); lab_exit(1);
    }
    __asm__ volatile ("csrw pmpaddr0, %0; csrw pmpaddr1, %1; csrw pmpcfg0, %2"
                      : : "r"(deny_top), "r"(allow), "r"(UINT64_C(0x1f08))
                      : "memory");
    uint64_t cfg, addr;
    __asm__ volatile ("csrr %0, pmpcfg0; csrr %1, pmpaddr1"
                      : "=r"(cfg), "=r"(addr));
    if ((cfg & UINT64_C(0xffff)) != UINT64_C(0x1f08) || addr != allow) {
        lab_puts("PMP readback mismatch\n"); lab_exit(1);
    }
}

static __attribute__((unused)) void install_smoke_pmp(void)
{
    uint64_t deny_top = LAB_S_BASE >> 2;
    uint64_t allow = (LAB_S_BASE >> 2) |
        (((LAB_S_END - LAB_S_BASE) >> 3) - 1);
    uint64_t cfg = UINT64_C(0x1f08);
    __asm__ volatile ("csrw pmpaddr0, %0; csrw pmpaddr1, %1; csrw pmpcfg0, %2"
                      : : "r"(deny_top), "r"(allow), "r"(cfg) : "memory");
}

void lab_trap_dispatch(lab_frame *frame)
{
    if (frame->mcause == 9) {
#if defined(LAB_SMOKE)
        failures += lab_report("S-mode ecall reaches M-mode", frame->a7 == LAB_SBI_EXPERIMENTAL);
        lab_puts(failures ? "SMOKE FAIL\n" : "SMOKE PASS\n");
        lab_exit(failures != 0);
#else
        uint64_t eid = frame->a7, fid = frame->a6, completion = frame->a0;
        sbi_calls++;
        lab_sbi_dispatch(frame);
        if (eid == LAB_SBI_EXPERIMENTAL && fid == 1) {
            failures += lab_report("S payload SBI sequence and ABI",
                completion == 1 && (int64_t)frame->a0 == LAB_SBI_SUCCESS &&
                frame->a1 == 0 && sbi_calls == 6);
            failures += lab_report("PMP blocks S write to firmware",
                pmp_violations == 1 &&
                lab_firmware_secret == UINT64_C(0xfeedfacec0decafe));
            lab_puts(failures ? "LAB15 INCOMPLETE\n" : "LAB15 PASS\n");
            lab_exit(failures != 0);
        }
        __asm__ volatile ("csrw mepc, %0" : : "r"(frame->mepc + 4));
        return;
#endif
    }
    if (frame->mcause == 7 &&
        frame->mtval == (uint64_t)(uintptr_t)&lab_firmware_secret) {
        pmp_violations++;
        __asm__ volatile ("csrw mepc, %0" : : "r"(frame->mepc + 4));
        return;
    }
    lab_puts("UNEXPECTED M TRAP mcause="); lab_puthex(frame->mcause);
    lab_puts(" mepc="); lab_puthex(frame->mepc);
    lab_puts(" mtval="); lab_puthex(frame->mtval); lab_puts("\n");
    lab_exit(1);
}

void lab_main(void)
{
#if defined(LAB_SMOKE)
    lab_puts("LAB15 M/S infrastructure smoke tests\n");
    install_smoke_pmp();
    lab_enter_s((uint64_t)(uintptr_t)lab_s_smoke,
                (uint64_t)(uintptr_t)__s_stack_top);
#else
    lab_puts("LAB15 M-mode SBI/domain unit tests\n");
    failures = run_units();
#if defined(LAB_UNIT)
    lab_puts(failures ? "UNIT INCOMPLETE\n" : "UNIT PASS\n");
    lab_exit(failures != 0);
#else
    if (failures) {
        lab_puts("LAB15 INCOMPLETE: S payload gated by unit tests\n");
        lab_exit(1);
    }
    install_pmp();
    lab_puts("LAB15 entering S-mode payload\n");
    lab_enter_s((uint64_t)(uintptr_t)lab_s_payload,
                (uint64_t)(uintptr_t)__s_stack_top);
#endif
#endif
    lab_exit(1);
}

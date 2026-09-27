#include "lab_api.h"

uint64_t lab_gstage_root[2048] __attribute__((aligned(16384)));
uint64_t lab_alias_l1[512] __attribute__((aligned(4096)));

static unsigned failures;
static unsigned alias_faults;
static unsigned alias_store_faults;
static unsigned mmio_faults;
static uint64_t mmio_value;
extern __attribute__((noreturn)) void lab_trap_resume(lab_frame *frame);

static __attribute__((unused)) unsigned run_units(void)
{
    unsigned failed = 0;
    uint64_t slot = 0;
    uint64_t flags = LAB_PTE_R | LAB_PTE_U | LAB_PTE_A;
    uint64_t pte = ((uint64_t)(uintptr_t)__guest_page >> 12 << 10) |
                   flags | LAB_PTE_V;
    failed += lab_report("Sv39x4 41-bit root index",
        lab_gstage_index(0) == 0 &&
        lab_gstage_index(LAB_GUEST_ALIAS) == 1 &&
        lab_gstage_index(UINT64_C(0x80000000)) == 2 &&
        lab_gstage_index((UINT64_C(1) << 41) - 1) == 2047 &&
        lab_gstage_index(UINT64_C(1) << 41) == LAB_INVALID);
    failed += lab_report("legal 2 MiB G-stage leaf",
        lab_gstage_leaf((uint64_t)(uintptr_t)__guest_page, flags) == pte &&
        lab_gstage_leaf((uint64_t)(uintptr_t)__guest_page + 4096, flags) == 0 &&
        lab_gstage_leaf((uint64_t)(uintptr_t)__guest_page, LAB_PTE_R) == 0 &&
        lab_gstage_leaf(UINT64_C(1) << 56, flags) == 0 &&
        lab_gstage_leaf((uint64_t)(uintptr_t)__guest_page,
                        LAB_PTE_W | LAB_PTE_U) == 0);
    failed += lab_report("guest fault provenance and GPA",
        lab_guest_fault(21, LAB_HSTATUS_SPV, LAB_GUEST_ALIAS >> 2,
                        LAB_GUEST_ALIAS) == 1 &&
        lab_guest_fault(23, LAB_HSTATUS_SPV, LAB_GUEST_MMIO >> 2,
                        LAB_GUEST_MMIO) == 2 &&
        lab_guest_fault(23, LAB_HSTATUS_SPV, LAB_GUEST_ALIAS >> 2,
                        LAB_GUEST_ALIAS) == 3 &&
        lab_guest_fault(21, 0, LAB_GUEST_ALIAS >> 2,
                        LAB_GUEST_ALIAS) == 0 &&
        lab_guest_fault(21, LAB_HSTATUS_SPV, 0,
                        LAB_GUEST_ALIAS) == 0);
    failed += lab_report("install mapping and local HFENCE.GVMA",
        lab_install_mapping(&slot, pte, LAB_GUEST_ALIAS) == 0 &&
        slot == pte &&
        lab_install_mapping(&slot, pte, LAB_GUEST_ALIAS) == pte &&
        lab_install_mapping(0, pte, LAB_GUEST_ALIAS) == LAB_INVALID &&
        lab_install_mapping(&slot, 1, LAB_GUEST_ALIAS) == LAB_INVALID &&
        lab_install_mapping(&slot, pte, LAB_GUEST_ALIAS + 4096) == LAB_INVALID &&
        slot == pte);
    return failed;
}

static __attribute__((unused)) void setup_guest(void)
{
    uint64_t identity = ((uint64_t)0x80000000 >> 12 << 10) |
        LAB_PTE_V | LAB_PTE_R | LAB_PTE_W | LAB_PTE_X |
        LAB_PTE_U | LAB_PTE_A | LAB_PTE_D;
    lab_gstage_root[2] = identity;
    lab_gstage_root[1] = ((uint64_t)(uintptr_t)lab_alias_l1 >> 12 << 10) |
                          LAB_PTE_V;
    *(volatile uint64_t *)(void *)__guest_page = LAB_GUEST_MAGIC;
    uint64_t root = ((uint64_t)(uintptr_t)lab_gstage_root >> 12) |
                    (UINT64_C(8) << 60);
    __asm__ volatile ("fence rw, rw; csrw hgatp, %0; hfence.gvma x0, x0"
                      : : "r"(root) : "memory");
}

void lab_trap_dispatch(lab_frame *frame)
{
    uint64_t htval, stval;
    __asm__ volatile ("csrr %0, htval" : "=r"(htval));
    __asm__ volatile ("csrr %0, stval" : "=r"(stval));
    uint64_t kind = lab_guest_fault(frame->scause, frame->hstatus,
                                    htval, stval);
    if (kind == 1 && alias_faults == 0) {
        uint64_t pte = lab_gstage_leaf((uint64_t)(uintptr_t)__guest_page,
                                      LAB_PTE_R | LAB_PTE_U | LAB_PTE_A);
        alias_faults++;
        if (pte != 0 && lab_install_mapping(&lab_alias_l1[0], pte,
                                             LAB_GUEST_ALIAS) == 0)
            lab_trap_resume(frame);
    }
    if (kind == 3 && alias_store_faults == 0 && alias_faults == 1) {
        alias_store_faults++;
        __asm__ volatile ("csrw sepc, %0" : : "r"(frame->sepc + 4));
        lab_trap_resume(frame);
    }
    if (kind == 2 && mmio_faults == 0 && alias_store_faults == 1) {
        mmio_faults++;
        mmio_value = frame->a0;
        __asm__ volatile ("csrw sepc, %0" : : "r"(frame->sepc + 4));
        lab_trap_resume(frame);
    }
    if (frame->scause == LAB_CAUSE_VS_ECALL &&
        (frame->hstatus & LAB_HSTATUS_SPV) != 0) {
        failures += lab_report("one real G-stage fault and HFENCE retry",
                               alias_faults == 1 && lab_alias_l1[0] != 0);
        failures += lab_report("read-only G-stage leaf blocks guest Store",
                               alias_store_faults == 1 &&
                               *(volatile uint64_t *)(void *)__guest_page ==
                               LAB_GUEST_MAGIC);
        failures += lab_report("MMIO fault classified and emulated",
                               mmio_faults == 1 &&
                               mmio_value == LAB_GUEST_MAGIC);
        lab_puts(failures ? "LAB14 INCOMPLETE\n" : "LAB14 PASS\n");
        lab_exit(failures != 0);
    }
    lab_puts("UNEXPECTED HS TRAP scause="); lab_puthex(frame->scause);
    lab_puts(" hstatus="); lab_puthex(frame->hstatus);
    lab_puts(" htval="); lab_puthex(htval);
    lab_puts(" stval="); lab_puthex(stval); lab_puts("\n");
    lab_exit(1);
}

void lab_main(void)
{
    uintptr_t stack;
    __asm__ volatile ("mv %0, sp" : "=r"(stack));
    __asm__ volatile ("csrw stvec, %0" : : "r"(lab_hs_trap));
#if defined(LAB_SMOKE)
    lab_puts("LAB14 infrastructure smoke tests\n");
    failures += lab_report("M-to-HS handoff and aligned stack",
                           (stack & 15u) == 0);
    failures += lab_report("16 KiB Sv39x4 root alignment",
                           ((uintptr_t)lab_gstage_root & 16383u) == 0);
    lab_puts(failures ? "SMOKE FAIL\n" : "SMOKE PASS\n");
    lab_exit(failures != 0);
#else
    lab_puts("LAB14 H-extension unit tests\n");
    failures = run_units();
#if defined(LAB_UNIT)
    lab_puts(failures ? "UNIT INCOMPLETE\n" : "UNIT PASS\n");
    lab_exit(failures != 0);
#else
    if (failures) {
        lab_puts("LAB14 INCOMPLETE: guest phase gated by unit tests\n");
        lab_exit(1);
    }
    setup_guest();
    lab_puts("LAB14 entering real VS guest\n");
    lab_enter_guest((uint64_t)(uintptr_t)lab_guest_entry,
                    (uint64_t)(uintptr_t)__guest_stack_top);
    lab_exit(1);
#endif
#endif
}

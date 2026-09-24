#include "lab_api.h"

static volatile uint64_t probe_seen;
static volatile uint64_t breakpoint_seen;

static int frame_is_on_trap_stack(const struct lab_pt_regs *regs)
{
    uintptr_t address = (uintptr_t)regs;
    uintptr_t bottom = (uintptr_t)__trap_stack_bottom;
    uintptr_t top = (uintptr_t)__trap_stack_top;

    return (address & 15u) == 0 && address >= bottom &&
           address <= top - sizeof(*regs);
}

static int user_sp_is_valid(uint64_t user_sp)
{
    uintptr_t bottom = (uintptr_t)__user_stack_bottom;
    uintptr_t top = (uintptr_t)__user_stack_top;

    return (user_sp & 15u) == 0 && user_sp >= bottom && user_sp <= top;
}

static int probe_frame_is_valid(const struct lab_pt_regs *regs)
{
    return frame_is_on_trap_stack(regs) &&
           user_sp_is_valid(regs->x[LAB_X_SP]) &&
           regs->scause == LAB_CAUSE_USER_ECALL && regs->stval == 0 &&
           (regs->sstatus & LAB_SSTATUS_SPP) == 0 &&
           (regs->flags & LAB_FRAME_FROM_USER) != 0 &&
           regs->hartid == 0 &&
           regs->orig_a0 == LAB_SENTINEL_A0 &&
           regs->x[LAB_X_RA] == LAB_SENTINEL_RA &&
           regs->x[LAB_X_GP] == LAB_SENTINEL_GP &&
           regs->x[LAB_X_TP] == LAB_SENTINEL_TP &&
           regs->x[LAB_X_T0] == LAB_SENTINEL_T0 &&
           regs->x[LAB_X_T1] == LAB_SENTINEL_T1 &&
           regs->x[LAB_X_T2] == LAB_SENTINEL_T2 &&
           regs->x[LAB_X_S0] == LAB_SENTINEL_S0 &&
           regs->x[LAB_X_S1] == LAB_SENTINEL_S1 &&
           regs->x[LAB_X_S11] == LAB_SENTINEL_S11 &&
           regs->x[LAB_X_A0] == LAB_SENTINEL_A0 &&
           regs->x[LAB_X_A1] == LAB_SENTINEL_A1 &&
           regs->x[LAB_X_A6] == LAB_SENTINEL_A6 &&
           regs->x[LAB_X_A7] == LAB_SYSCALL_PROBE;
}

static uint64_t next_epc_or_finish(struct lab_pt_regs *regs)
{
    uint16_t first_halfword =
        *(const volatile uint16_t *)(uintptr_t)regs->sepc;
    uint64_t next = lab_advance_epc(regs->sepc, first_halfword);

    if (next == UINT64_MAX) {
        lab_report("safe EPC advance", 0);
        lab_puts("LAB10 INCOMPLETE\n");
        lab_exit(1);
    }
    return next;
}

static void finish_actual_probe(int user_reported_success)
{
    int passed = probe_seen != 0 && breakpoint_seen != 0 &&
                 user_reported_success;

    lab_full_failures += lab_report(
        "U-mode ecall/breakpoint full-frame round trip", passed);
    lab_puts(lab_full_failures == 0 ? "LAB10 PASS\n" :
                                      "LAB10 INCOMPLETE\n");
    lab_exit(lab_full_failures == 0 ? 0u : 1u);
}

void __attribute__((section(".text.flow")))
lab_s_trap_dispatch(struct lab_pt_regs *regs)
{
    if (regs == 0 || !frame_is_on_trap_stack(regs)) {
        finish_actual_probe(0);
    }
    if (regs->scause == LAB_CAUSE_USER_ECALL) {
        if (regs->x[LAB_X_A7] == LAB_SYSCALL_PROBE) {
            probe_seen = probe_frame_is_valid(regs);
            regs->x[LAB_X_A0] = LAB_SYSCALL_RETURN_MAGIC;
            regs->sepc = next_epc_or_finish(regs);
            return;
        }
        if (regs->x[LAB_X_A7] == LAB_SYSCALL_EXIT) {
            finish_actual_probe(regs->x[LAB_X_A0] == 0);
        }
        finish_actual_probe(0);
    }
    if (regs->scause == LAB_CAUSE_BREAKPOINT) {
        uint32_t instruction =
            *(const volatile uint32_t *)(uintptr_t)regs->sepc;
        breakpoint_seen = probe_seen != 0 &&
                          instruction == UINT32_C(0x00100073) &&
                          (regs->flags & LAB_FRAME_FROM_USER) != 0;
        regs->sepc = next_epc_or_finish(regs);
        return;
    }
    finish_actual_probe(0);
}

void lab_trap_unimplemented(void)
{
    lab_full_failures += lab_report("S-mode trap vector implementation", 0);
    lab_puts("LAB10 INCOMPLETE\n");
    lab_exit(1);
}

void lab_m_trap_fail(uint64_t cause, uint64_t epc, uint64_t tval)
{
    lab_puts("[FAIL] unexpected M-mode trap cause=");
    lab_puthex(cause);
    lab_puts(" epc=");
    lab_puthex(epc);
    lab_puts(" tval=");
    lab_puthex(tval);
    lab_puts("\nLAB10 INCOMPLETE\n");
    lab_exit(1);
}

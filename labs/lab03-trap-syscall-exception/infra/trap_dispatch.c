#include <stddef.h>

#include "lab_api.h"

_Static_assert(sizeof(struct lab_trap_frame) == TF_SIZE,
               "trap frame size disagrees with assembly");
_Static_assert(offsetof(struct lab_trap_frame, x[10]) == TF_A0,
               "a0 offset disagrees with assembly");
_Static_assert(offsetof(struct lab_trap_frame, sstatus) == TF_SSTATUS,
               "sstatus offset disagrees with assembly");

static unsigned int trap_count;
static unsigned int breakpoint_count;
static unsigned int illegal_count;
static unsigned int frame_failures;

static int frame_invariants_hold(const struct lab_trap_frame *frame)
{
    uintptr_t user_sp = (uintptr_t)frame->x[2];
    int sentinels = frame->x[3] == UINT64_C(LAB_USER_GP_SENTINEL) &&
        frame->x[4] == UINT64_C(LAB_USER_TP_SENTINEL) &&
        frame->x[20] == 0x54 && frame->x[21] == 0x55 &&
        frame->x[22] == 0x56 && frame->x[23] == 0x57 &&
        frame->x[24] == 0x58 && frame->x[25] == 0x59 &&
        frame->x[26] == 0x5a && frame->x[27] == 0x5b;

    return frame->x[0] == 0 && (frame->sstatus & (UINT64_C(1) << 8)) == 0 &&
        user_sp >= (uintptr_t)__u_stack_bottom &&
        user_sp <= (uintptr_t)__u_stack_top && sentinels;
}

static void finish_from_user(struct lab_trap_frame *frame)
{
    unsigned int failures = 0;
    lab_puts("LAB03 U-to-S trap tests\n");
    failures += lab_report("six U-mode traps reached S-mode", trap_count == 6);
    failures += lab_report("trap frame preserved user state", frame_failures == 0);
    failures += lab_report("ADD syscall returned through saved a0",
                           frame->x[10] == UINT64_C(0x68ac));
    failures += lab_report("XOR syscall returned through saved a0",
                           frame->x[11] == UINT64_C(0xf0f0));
    failures += lab_report("16-bit and 32-bit breakpoints resumed",
                           breakpoint_count == 2);
    failures += lab_report("illegal instruction wrote selected rd",
                           illegal_count == 1 &&
                           frame->x[12] == UINT64_C(LAB_EMULATED_VALUE));
    failures += lab_run_helper_tests();
    lab_puts(failures == 0 ? "LAB03 PASS\n" : "LAB03 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

__attribute__((section(".text.trap_runtime")))
void lab_s_trap_dispatch(struct lab_trap_frame *frame)
{
    uint64_t cause = frame->scause;
    unsigned long length;

    ++trap_count;
    if (!frame_invariants_hold(frame)) {
        ++frame_failures;
    }

    if ((cause >> 63) != 0) {
        lab_puts("[FAIL] unexpected interrupt\n");
        lab_exit(1);
    }

    switch (cause) {
    case LAB_SCAUSE_USER_ECALL:
        if (frame->x[17] == LAB_SYSCALL_FINISH) {
            finish_from_user(frame);
        }
        if (lab_handle_syscall(frame) != 0) {
            lab_puts("[FAIL] unknown syscall\n");
            lab_exit(1);
        }
        frame->sepc += 4;
        return;
    case LAB_SCAUSE_BREAKPOINT:
        length = lab_instruction_length((const void *)(uintptr_t)frame->sepc);
        if (length != 2 && length != 4) {
            lab_puts("[FAIL] invalid breakpoint instruction length\n");
            lab_exit(1);
        }
        frame->sepc += length;
        ++breakpoint_count;
        return;
    case LAB_SCAUSE_ILLEGAL_INSTRUCTION:
        if (lab_emulate_instruction(frame) != 0) {
            lab_puts("[FAIL] unhandled illegal instruction\n");
            lab_exit(1);
        }
        ++illegal_count;
        return;
    default:
        lab_puts("[FAIL] unexpected synchronous exception: ");
        lab_puthex(cause);
        lab_puts("\n");
        lab_exit(1);
    }
}

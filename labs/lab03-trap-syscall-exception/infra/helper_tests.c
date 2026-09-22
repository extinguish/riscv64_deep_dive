#include <stddef.h>

#include "lab_api.h"

_Static_assert(sizeof(struct lab_trap_frame) == TF_SIZE,
               "trap frame size disagrees with assembly");
_Static_assert(offsetof(struct lab_trap_frame, sepc) == TF_SEPC,
               "sepc offset disagrees with assembly");
_Static_assert(offsetof(struct lab_trap_frame, scause) == TF_SCAUSE,
               "scause offset disagrees with assembly");

unsigned int lab_run_helper_tests(void)
{
    static const uint16_t compressed = UINT16_C(0x9002);
    static const uint16_t normal = UINT16_C(0x0073);
    static const uint16_t long_encoding = UINT16_C(0x001f);
    uint32_t custom_instruction = (15u << 7) | 0x0bu;
    uint32_t unknown_instruction = UINT32_C(0xffffffff);
    struct lab_trap_frame frame = {0};
    unsigned int failures = 0;
    int passed;

    passed = lab_instruction_length(&compressed) == 2 &&
             lab_instruction_length(&normal) == 4 &&
             lab_instruction_length(&long_encoding) == 0;
    failures += lab_report("16/32/long instruction length decode", passed);

    frame.x[17] = LAB_SYSCALL_ADD;
    frame.x[10] = UINT64_C(0x1234);
    frame.x[11] = UINT64_C(0x5678);
    passed = lab_handle_syscall(&frame) == 0 &&
             frame.x[10] == UINT64_C(0x68ac);
    frame.x[17] = LAB_SYSCALL_XOR;
    frame.x[10] = UINT64_C(0xff00);
    frame.x[11] = UINT64_C(0x0ff0);
    passed = passed && lab_handle_syscall(&frame) == 0 &&
             frame.x[10] == UINT64_C(0xf0f0);
    frame.x[17] = 99;
    passed = passed && lab_handle_syscall(&frame) == -1;
    failures += lab_report("syscall dispatch and saved a0 writeback", passed);

    frame.sepc = (uintptr_t)&custom_instruction;
    frame.x[15] = 0;
    passed = lab_emulate_instruction(&frame) == 0 &&
             frame.x[15] == UINT64_C(LAB_EMULATED_VALUE) &&
             frame.sepc == (uintptr_t)&custom_instruction + 4;
    frame.sepc = (uintptr_t)&unknown_instruction;
    passed = passed && lab_emulate_instruction(&frame) == -1 &&
             frame.sepc == (uintptr_t)&unknown_instruction;
    failures += lab_report("custom illegal-instruction emulation", passed);
    return failures;
}

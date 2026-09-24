#include "lab_api.h"

_Static_assert(sizeof(struct lab_switch_context) == LAB_CTX_SIZE,
               "assembly switch-context size mismatch");
_Static_assert(__builtin_offsetof(struct lab_switch_context, s[0]) == LAB_CTX_S0,
               "assembly s0 offset mismatch");
_Static_assert(__builtin_offsetof(struct lab_switch_context, s[11]) == LAB_CTX_S11,
               "assembly s11 offset mismatch");
_Static_assert(__builtin_offsetof(struct lab_switch_context, tp) == LAB_CTX_TP,
               "assembly tp offset mismatch");

static uint8_t init_stack[1024] __attribute__((aligned(64)));
static struct lab_switch_context init_context;

static uint64_t init_entry(uint64_t argument)
{
    return argument;
}

static void init_exit(uint64_t result)
{
    (void)result;
    for (;;) {
    }
}

static int test_context_initialization(void)
{
    uintptr_t base = (uintptr_t)init_stack;
    uintptr_t expected_sp = (base + sizeof(init_stack)) & ~(uintptr_t)15;

    lab_memzero(&init_context, sizeof(init_context));
    if (!lab_context_init(&init_context, base, sizeof(init_stack),
                          init_entry, LAB_TASK_ARGUMENT, init_exit,
                          LAB_TASK_TP)) return 0;
    if (init_context.ra != (uintptr_t)lab_task_trampoline ||
        init_context.sp != expected_sp ||
        init_context.s[0] != (uintptr_t)init_entry ||
        init_context.s[1] != LAB_TASK_ARGUMENT ||
        init_context.s[2] != (uintptr_t)init_exit ||
        init_context.s[3] != 0 || init_context.s[11] != 0 ||
        init_context.tp != LAB_TASK_TP ||
        init_context.state != LAB_CTX_READY) return 0;
    return !lab_context_init(0, base, sizeof(init_stack), init_entry,
                             0, init_exit, 0) &&
           !lab_context_init(&init_context, 0, sizeof(init_stack),
                             init_entry, 0, init_exit, 0) &&
           !lab_context_init(&init_context, base, LAB_TASK_STACK_MIN - 1,
                             init_entry, 0, init_exit, 0) &&
           !lab_context_init(&init_context, UINTPTR_MAX - 127, 512,
                             init_entry, 0, init_exit, 0) &&
           !lab_context_init(&init_context, base, sizeof(init_stack), 0,
                             0, init_exit, 0) &&
           !lab_context_init(&init_context, base, sizeof(init_stack),
                             init_entry, 0, 0, 0) &&
           !lab_context_init(&init_context, base + 1, LAB_TASK_STACK_MIN,
                             init_entry, 0, init_exit, 0);
}

static int test_vector_context_sizing(void)
{
    return lab_vector_context_size(16) == 576 &&
           lab_vector_context_size(64) == 2112 &&
           lab_vector_context_size(65536) == UINT64_C(2097216) &&
           lab_vector_context_size(0) == UINT64_MAX &&
           lab_vector_context_size(24) == UINT64_MAX &&
           lab_vector_context_size(8) == UINT64_MAX &&
           lab_vector_context_size(131072) == UINT64_MAX;
}

static int test_extension_plan(void)
{
    uint64_t outgoing = LAB_EXT_DIRTY |
                        (LAB_EXT_CLEAN << LAB_EXT_VS_SHIFT);
    uint64_t incoming = LAB_EXT_CLEAN |
                        (LAB_EXT_OFF << LAB_EXT_VS_SHIFT);
    uint64_t both_dirty = LAB_EXT_DIRTY |
                          (LAB_EXT_DIRTY << LAB_EXT_VS_SHIFT);
    uint64_t both_live = LAB_EXT_INITIAL |
                         (LAB_EXT_CLEAN << LAB_EXT_VS_SHIFT);

    return lab_extension_switch_plan(outgoing, incoming, 0) ==
               (LAB_ACTION_SAVE_FPU | LAB_ACTION_RESTORE_FPU |
                LAB_ACTION_DISABLE_VECTOR) &&
           lab_extension_switch_plan(both_dirty, both_live,
                                     LAB_OWNER_MASK) ==
               (LAB_ACTION_SAVE_FPU | LAB_ACTION_SAVE_VECTOR) &&
           lab_extension_switch_plan(0, 0, 0) ==
               (LAB_ACTION_DISABLE_FPU | LAB_ACTION_DISABLE_VECTOR) &&
           lab_extension_switch_plan(0x10, 0, 0) == UINT64_MAX &&
           lab_extension_switch_plan(0, 0x10, 0) == UINT64_MAX &&
           lab_extension_switch_plan(0, 0, 4) == UINT64_MAX;
}

static int test_migration_policy(void)
{
    uint64_t base_isa = LAB_ISA_I | LAB_ISA_M | LAB_ISA_A;
    uint64_t vector_isa = base_isa | LAB_ISA_V;

    return lab_migration_allowed(base_isa, 0, 0, vector_isa, 64, 0) &&
           lab_migration_allowed(vector_isa, 64,
                                 LAB_TASK_VECTOR_ACTIVE, vector_isa,
                                 64, 0) &&
           !lab_migration_allowed(vector_isa, 64,
                                  LAB_TASK_VECTOR_ACTIVE, base_isa,
                                  0, 0) &&
           !lab_migration_allowed(vector_isa, 64,
                                  LAB_TASK_VECTOR_ACTIVE, vector_isa,
                                  128, 0) &&
           !lab_migration_allowed(base_isa, 0,
                                  LAB_TASK_MIGRATE_DISABLED, vector_isa,
                                  64, 0) &&
           !lab_migration_allowed(base_isa, 0, LAB_TASK_LAZY_OWNER,
                                  vector_isa, 64, 0) &&
           lab_migration_allowed(base_isa, 0, LAB_TASK_LAZY_OWNER,
                                 vector_isa, 64,
                                 LAB_TARGET_OWNER_HANDOFF) &&
           !lab_migration_allowed(base_isa, 16, 0,
                                  vector_isa, 64, 0) &&
           !lab_migration_allowed(base_isa, 0, 8,
                                  vector_isa, 64, 0) &&
           !lab_migration_allowed(base_isa, 0, 0,
                                  vector_isa, 64, 2);
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;
    failures += lab_report("synthetic first-run context", test_context_initialization());
    failures += lab_report("variable VLEN context sizing", test_vector_context_sizing());
    failures += lab_report("FS/VS lazy save and restore plan", test_extension_plan());
    failures += lab_report("heterogeneous-hart migration policy", test_migration_policy());
    return failures;
}

#include "lab_api.h"

struct lab_switch_context lab_scheduler_context;
struct lab_switch_context lab_task_context;
volatile uint64_t lab_task_phase;
volatile uint64_t lab_task_result;
volatile uint64_t lab_task_probe_failed;

static int test_context_round_trip(void)
{
    lab_memzero(&lab_scheduler_context, sizeof(lab_scheduler_context));
    lab_memzero(&lab_task_context, sizeof(lab_task_context));
    lab_task_phase = 0;
    lab_task_result = 0;
    lab_task_probe_failed = 0;
    if (!lab_context_init(&lab_task_context,
                          (uintptr_t)__task_stack_bottom, LAB_STACK_SIZE,
                          lab_task_probe, LAB_TASK_ARGUMENT, lab_task_exit,
                          LAB_TASK_TP)) return 0;
    lab_task_context.state = LAB_CTX_RUNNING;
    lab_scheduler_context.state = LAB_CTX_RUNNING;
    lab_context_switch(&lab_scheduler_context, &lab_task_context);
    if (lab_task_phase != 1 || lab_task_probe_failed != 0 ||
        lab_task_context.state != LAB_CTX_RUNNING) return 0;
    lab_context_switch(&lab_scheduler_context, &lab_task_context);
    return lab_task_phase == 2 && lab_task_probe_failed == 0 &&
           lab_task_result == LAB_TASK_RESULT &&
           lab_task_context.state == LAB_CTX_EXITED;
}

void lab_task_unexpected_return(void)
{
    lab_puts("[FAIL] exited task resumed unexpectedly\nLAB11 INCOMPLETE\n");
    lab_exit(1);
}

void lab_full_main(uint64_t hartid)
{
    unsigned int failures;
    (void)hartid;
    lab_puts("LAB11 context-switch and task-state tests\n");
    failures = lab_run_unit_tests();
    failures += lab_report("scheduler/task yield, resume, and exit",
                           test_context_round_trip());
    lab_puts(failures ? "LAB11 INCOMPLETE\n" : "LAB11 PASS\n");
    lab_exit(failures ? 1u : 0u);
}

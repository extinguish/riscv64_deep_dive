#include "lab_api.h"

void lab_unit_main(uint64_t hartid)
{
    unsigned int failures;
    (void)hartid;
    lab_pmp_baseline_init();
    lab_puts("LAB07 PMP model unit tests\n");
    failures = lab_run_unit_tests();
    lab_puts(failures == 0 ? "UNIT PASS\n" : "UNIT INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

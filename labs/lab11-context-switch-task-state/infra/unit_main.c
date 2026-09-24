#include "lab_api.h"
void lab_unit_main(uint64_t hartid)
{
    unsigned int failures;
    (void)hartid;
    lab_puts("LAB11 context and extension-state unit tests\n");
    failures = lab_run_unit_tests();
    lab_puts(failures ? "UNIT INCOMPLETE\n" : "UNIT PASS\n");
    lab_exit(failures ? 1u : 0u);
}

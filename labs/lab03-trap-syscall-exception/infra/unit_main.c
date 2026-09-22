#include "lab_api.h"

void lab_unit_main(void)
{
    unsigned int failures;
    lab_puts("LAB03 helper unit tests\n");
    failures = lab_run_helper_tests();
    lab_puts(failures == 0 ? "UNIT PASS\n" : "UNIT INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

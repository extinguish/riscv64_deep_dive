#include "lab_api.h"

static int test_memory_helpers(void)
{
    uint8_t source[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t destination[8];
    uint64_t index;

    lab_memzero(destination, sizeof(destination));
    lab_memcpy(destination, source, sizeof(source));
    for (index = 0; index < sizeof(source); ++index) {
        if (destination[index] != source[index]) {
            return 0;
        }
    }
    return 1;
}

static int test_trace_backend(void)
{
    lab_trace_reset();
    if (!lab_cmo_emit(LAB_CMO_CLEAN, UINT64_C(0x1000)) ||
        !lab_cmo_emit(LAB_CMO_FLUSH, UINT64_C(0x1040))) {
        return 0;
    }
    __asm__ volatile ("fence rw,rw; fence.i" ::: "memory");
    return lab_trace_count == 2 &&
           lab_trace_events[0].operation == LAB_CMO_CLEAN &&
           lab_trace_events[0].address == UINT64_C(0x1000) &&
           lab_trace_events[1].operation == LAB_CMO_FLUSH &&
           lab_trace_events[1].address == UINT64_C(0x1040);
}

void lab_smoke_main(uint64_t hartid)
{
    unsigned int failures = 0;
    (void)hartid;

    lab_puts("LAB08 infrastructure smoke tests\n");
    failures += lab_report("UART, stack, and memory helpers",
                           test_memory_helpers());
    failures += lab_report("CMO trace backend and fence.i availability",
                           test_trace_backend());
    lab_puts(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

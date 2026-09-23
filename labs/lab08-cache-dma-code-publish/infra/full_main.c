#include "lab_api.h"

static uint32_t jit_buffer[LAB_JIT_MAX_WORDS]
    __attribute__((section(".jit_buffer"),
                   aligned(LAB_CACHE_BLOCK_SIZE)));

static int test_local_code_publication(void)
{
    static const uint32_t return_42[] = {
        UINT32_C(0x02a00513), /* addi a0, zero, 42 */
        UINT32_C(0x00008067)  /* jalr zero, ra, 0 */
    };
    static const uint32_t return_99[] = {
        UINT32_C(0x06300513), /* addi a0, zero, 99 */
        UINT32_C(0x00008067)  /* jalr zero, ra, 0 */
    };

    if (lab_publish_code(0, return_42, 2) ||
        lab_publish_code(jit_buffer, return_42, 0) ||
        lab_publish_code(jit_buffer, return_42,
                         LAB_JIT_MAX_WORDS + 1) ||
        lab_publish_code((uint32_t *)((uintptr_t)jit_buffer + 2),
                         return_42, 2) ||
        lab_publish_code(jit_buffer,
                         (const uint32_t *)((uintptr_t)return_42 + 2), 2)) {
        return 0;
    }
    if (!lab_publish_code(jit_buffer, return_42, 2) ||
        lab_call_generated(jit_buffer) != 42) {
        return 0;
    }
    return lab_publish_code(jit_buffer, return_99, 2) &&
           lab_call_generated(jit_buffer) == 99;
}

void lab_full_main(uint64_t hartid)
{
    unsigned int failures = 0;
    (void)hartid;

    lab_puts("LAB08 cache maintenance, DMA, and code publication tests\n");
    failures += lab_report("local generated-code publication with fence.i",
                           test_local_code_publication());
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB08 PASS\n" : "LAB08 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

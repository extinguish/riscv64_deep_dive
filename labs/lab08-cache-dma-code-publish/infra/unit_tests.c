#include "lab_api.h"

_Static_assert(sizeof(struct lab_cmo_plan) == LAB_CMO_PLAN_SIZE,
               "assembly CMO-plan layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_cmo_plan, tail_partial) ==
                   LAB_CMO_PLAN_TAIL_PARTIAL,
               "assembly CMO-plan offsets mismatch");
_Static_assert(sizeof(struct lab_dma_desc) == LAB_DMA_DESC_SIZE,
               "assembly DMA-descriptor layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_dma_desc, owner) ==
                   LAB_DMA_DESC_OWNER,
               "assembly DMA-descriptor offsets mismatch");
_Static_assert(sizeof(struct lab_dma_channel) == LAB_DMA_CHANNEL_SIZE,
               "assembly DMA-channel layout mismatch");
_Static_assert(__builtin_offsetof(struct lab_dma_channel, completion) ==
                   LAB_DMA_CHANNEL_COMPLETION,
               "assembly DMA-channel offsets mismatch");

static uint8_t dma_buffer[128]
    __attribute__((aligned(LAB_CACHE_BLOCK_SIZE)));
static struct lab_dma_desc dma_descriptor;
static struct lab_dma_channel dma_channel;

static int event_is(uint64_t index, uint64_t operation, uint64_t address)
{
    return index < lab_trace_count &&
           lab_trace_events[index].operation == operation &&
           lab_trace_events[index].address == address;
}

static int test_range_normalization(void)
{
    struct lab_cmo_plan plan;

    if (!lab_normalize_cmo_range(UINT64_C(0x1003), 80, 64, &plan) ||
        plan.start != UINT64_C(0x1000) ||
        plan.end != UINT64_C(0x1080) || plan.blocks != 2 ||
        plan.head_partial != 1 || plan.tail_partial != 1) {
        return 0;
    }
    if (!lab_normalize_cmo_range(UINT64_C(0x2000), 64, 64, &plan) ||
        plan.start != UINT64_C(0x2000) ||
        plan.end != UINT64_C(0x2040) || plan.blocks != 1 ||
        plan.head_partial != 0 || plan.tail_partial != 0) {
        return 0;
    }
    if (!lab_normalize_cmo_range(UINT64_C(0x3007), 0, 64, &plan) ||
        plan.start != UINT64_C(0x3007) ||
        plan.end != UINT64_C(0x3007) || plan.blocks != 0 ||
        plan.head_partial != 0 || plan.tail_partial != 0) {
        return 0;
    }
    return !lab_normalize_cmo_range(UINT64_C(0x1000), 64, 48, &plan) &&
           !lab_normalize_cmo_range(UINT64_MAX - 7, 16, 64, &plan) &&
           !lab_normalize_cmo_range(UINT64_MAX - 31, 16, 64, &plan) &&
           !lab_normalize_cmo_range(UINT64_C(0x1000), 64, 64, 0);
}

static int test_cmo_policy_and_trace(void)
{
    lab_trace_reset();
    if (!lab_cmo_apply(UINT64_C(0x1003), 80, 64,
                       LAB_CMO_CLEAN, 0) || lab_trace_count != 2 ||
        !event_is(0, LAB_CMO_CLEAN, UINT64_C(0x1000)) ||
        !event_is(1, LAB_CMO_CLEAN, UINT64_C(0x1040))) {
        return 0;
    }

    lab_trace_reset();
    if (lab_cmo_apply(UINT64_C(0x1003), 80, 64,
                      LAB_CMO_INVALIDATE, 0) || lab_trace_count != 0) {
        return 0;
    }
    if (!lab_cmo_apply(UINT64_C(0x1003), 80, 64,
                       LAB_CMO_INVALIDATE, 1) || lab_trace_count != 2 ||
        !event_is(0, LAB_CMO_INVALIDATE, UINT64_C(0x1000)) ||
        !event_is(1, LAB_CMO_INVALIDATE, UINT64_C(0x1040))) {
        return 0;
    }

    lab_trace_reset();
    if (lab_cmo_apply(UINT64_C(0x4000),
                      (LAB_TRACE_MAX_EVENTS + 1) * 64, 64,
                      LAB_CMO_CLEAN, 1) ||
        lab_trace_count != LAB_TRACE_MAX_EVENTS) {
        return 0;
    }

    lab_trace_reset();
    return !lab_cmo_apply(UINT64_C(0x1000), 64, 64, 0, 1) &&
           lab_trace_count == 0;
}

static int test_dma_tx_protocol(void)
{
    uint64_t buffer_address = (uintptr_t)dma_buffer;
    uint64_t descriptor_address = (uintptr_t)&dma_descriptor;

    lab_memzero(dma_buffer, sizeof(dma_buffer));
    lab_memzero(&dma_descriptor, sizeof(dma_descriptor));
    lab_memzero(&dma_channel, sizeof(dma_channel));
    lab_trace_reset();

    if (lab_dma_prepare_tx(0, &dma_descriptor, dma_buffer, 96,
                           LAB_CACHE_BLOCK_SIZE) ||
        lab_trace_count != 0 || dma_channel.doorbell != 0) {
        return 0;
    }
    if (!lab_dma_prepare_tx(&dma_channel, &dma_descriptor, dma_buffer,
                            96, LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }
    return dma_descriptor.address == buffer_address &&
           dma_descriptor.length == 96 &&
           dma_descriptor.owner == LAB_DMA_OWNER_DEVICE &&
           dma_descriptor.status == 0 && dma_channel.doorbell == 1 &&
           lab_trace_count == 3 &&
           event_is(0, LAB_CMO_CLEAN, buffer_address) &&
           event_is(1, LAB_CMO_CLEAN,
                    buffer_address + LAB_CACHE_BLOCK_SIZE) &&
           event_is(2, LAB_CMO_CLEAN, descriptor_address);
}

static int test_dma_rx_protocol(void)
{
    uint64_t buffer_address = (uintptr_t)dma_buffer;

    lab_memzero(&dma_descriptor, sizeof(dma_descriptor));
    lab_memzero(&dma_channel, sizeof(dma_channel));
    dma_buffer[0] = UINT8_C(0xa5);
    dma_descriptor.address = buffer_address;
    dma_descriptor.length = sizeof(dma_buffer);
    dma_descriptor.owner = LAB_DMA_OWNER_CPU;
    dma_descriptor.status = 0;
    dma_channel.completion = 1;
    lab_trace_reset();

    if (!lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                             sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE) ||
        dma_channel.completion != 0 || dma_buffer[0] != UINT8_C(0xa5) ||
        lab_trace_count != 2 ||
        !event_is(0, LAB_CMO_INVALIDATE, buffer_address) ||
        !event_is(1, LAB_CMO_INVALIDATE,
                  buffer_address + LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }

    dma_descriptor.owner = LAB_DMA_OWNER_DEVICE;
    dma_channel.completion = 1;
    lab_trace_reset();
    if (lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                            sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }
    dma_descriptor.owner = LAB_DMA_OWNER_CPU;
    dma_descriptor.status = 1;
    if (lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                            sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }
    dma_descriptor.status = 0;
    dma_descriptor.address = buffer_address + LAB_CACHE_BLOCK_SIZE;
    if (lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                            sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }
    dma_descriptor.address = buffer_address;
    dma_descriptor.length = sizeof(dma_buffer) - 1;
    if (lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                            sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE)) {
        return 0;
    }
    dma_descriptor.length = sizeof(dma_buffer);
    dma_channel.completion = 0;
    return !lab_dma_complete_rx(&dma_channel, &dma_descriptor, dma_buffer,
                                sizeof(dma_buffer), LAB_CACHE_BLOCK_SIZE) &&
           lab_trace_count == 0;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("CMO range alignment and overflow checks",
                           test_range_normalization());
    failures += lab_report("partial-block policy and CMO trace",
                           test_cmo_policy_and_trace());
    failures += lab_report("DMA TX clean/descriptor/doorbell order",
                           test_dma_tx_protocol());
    failures += lab_report("DMA RX ownership and invalidate protocol",
                           test_dma_rx_protocol());
    return failures;
}

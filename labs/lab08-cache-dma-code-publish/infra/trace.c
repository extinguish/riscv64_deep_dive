#include "lab_api.h"

volatile struct lab_trace_event lab_trace_events[LAB_TRACE_MAX_EVENTS];
volatile uint64_t lab_trace_count;

void lab_trace_reset(void)
{
    uint64_t index;

    lab_trace_count = 0;
    for (index = 0; index < LAB_TRACE_MAX_EVENTS; ++index) {
        lab_trace_events[index].operation = 0;
        lab_trace_events[index].address = 0;
    }
}

int __attribute__((section(".text.flow")))
lab_cmo_emit(uint64_t operation, uint64_t address)
{
    uint64_t index = lab_trace_count;

    if (index >= LAB_TRACE_MAX_EVENTS) {
        return 0;
    }
    lab_trace_events[index].operation = operation;
    lab_trace_events[index].address = address;
    __asm__ volatile ("fence w,w" ::: "memory");
    lab_trace_count = index + 1;
    return 1;
}

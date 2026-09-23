#include "lab_api.h"

#define STRESS_TIMEOUT_CYCLES UINT64_C(100000000)

static volatile uint64_t barrier_count;
static volatile uint64_t barrier_sense;
static volatile uint64_t failure_bits;

static struct lab_mcs_lock mcs_lock;
static struct lab_mcs_node mcs_nodes[LAB_HART_COUNT];
static volatile uint64_t mcs_counter;
static volatile uint64_t mcs_in_critical;

static struct lab_spsc_ring spsc_ring;
static struct lab_seq_data seq_data;

static struct lab_stack stack;
static struct lab_stack_node stack_nodes[LAB_STACK_NODES];
static volatile uintptr_t hazard_slots[LAB_HART_COUNT]
    __attribute__((aligned(LAB_CACHELINE_SIZE)));
static volatile uint64_t popped_bitmap;

static struct lab_qsbr qsbr_domain;
static volatile uint64_t grace_target;

static uint64_t infra_amoadd(volatile uint64_t *address, uint64_t value)
{
    uint64_t old;
    __asm__ volatile ("amoadd.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(address), "r"(value) : "memory");
    return old;
}

static uint64_t infra_amoor(volatile uint64_t *address, uint64_t value)
{
    uint64_t old;
    __asm__ volatile ("amoor.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(address), "r"(value) : "memory");
    return old;
}

static void infra_or_failure(uint64_t bits)
{
    (void)infra_amoor(&failure_bits, bits);
}

static uint64_t read_cycle(void)
{
    uint64_t value;
    __asm__ volatile ("rdcycle %0" : "=r"(value));
    return value;
}

static void infra_barrier(uint64_t *local_sense)
{
    uint64_t expected = *local_sense ^ 1u;
    uint64_t old = infra_amoadd(&barrier_count, 1);
    if (old + 1 == LAB_HART_COUNT) {
        barrier_count = 0;
        __asm__ volatile ("fence rw, w" ::: "memory");
        barrier_sense = expected;
    } else {
        while (barrier_sense != expected) {
        }
        __asm__ volatile ("fence r, rw" ::: "memory");
    }
    *local_sense = expected;
}

static void hold_critical_window(void)
{
    volatile unsigned int count;
    for (count = 0; count < 48; ++count) {
        __asm__ volatile ("nop");
    }
}

static void run_mcs_stage(uint64_t hartid, uint64_t *local_sense)
{
    uint64_t index;

    for (index = 0; index < LAB_MCS_ITERATIONS; ++index) {
        lab_mcs_lock(&mcs_lock, &mcs_nodes[hartid]);
        if (mcs_in_critical != 0) {
            infra_or_failure(UINT64_C(1) << 0);
        }
        mcs_in_critical = 1;
        hold_critical_window();
        mcs_counter = mcs_counter + 1;
        mcs_in_critical = 0;
        lab_mcs_unlock(&mcs_lock, &mcs_nodes[hartid]);
    }
    infra_barrier(local_sense);
    if (hartid == 0 &&
        (mcs_counter != LAB_HART_COUNT * LAB_MCS_ITERATIONS ||
         mcs_lock.tail != 0)) {
        infra_or_failure(UINT64_C(1) << 0);
    }
    infra_barrier(local_sense);
}

static void run_spsc_stage(uint64_t hartid, uint64_t *local_sense)
{
    uint64_t index;
    uint64_t value = 0;
    uint64_t start;
    int complete = 1;

    if (hartid == 0) {
        for (index = 1; index <= LAB_SPSC_ITEMS; ++index) {
            start = read_cycle();
            while (!lab_spsc_try_push(&spsc_ring, index)) {
                if (read_cycle() - start > STRESS_TIMEOUT_CYCLES) {
                    infra_or_failure(UINT64_C(1) << 1);
                    complete = 0;
                    break;
                }
            }
            if (!complete) {
                break;
            }
        }
    } else if (hartid == 1) {
        for (index = 1; index <= LAB_SPSC_ITEMS; ++index) {
            start = read_cycle();
            while (!lab_spsc_try_pop(&spsc_ring, &value)) {
                if (read_cycle() - start > STRESS_TIMEOUT_CYCLES) {
                    infra_or_failure(UINT64_C(1) << 1);
                    complete = 0;
                    break;
                }
            }
            if (!complete) {
                break;
            }
            if (value != index) {
                infra_or_failure(UINT64_C(1) << 1);
            }
        }
    }
    infra_barrier(local_sense);
    if (hartid == 0 &&
        (spsc_ring.head != LAB_SPSC_ITEMS ||
         spsc_ring.tail != LAB_SPSC_ITEMS)) {
        infra_or_failure(UINT64_C(1) << 1);
    }
    infra_barrier(local_sense);
}

static void run_seqlock_stage(uint64_t hartid, uint64_t *local_sense)
{
    uint64_t index;
    uint64_t start_version;
    uint64_t x;
    uint64_t y;
    uint64_t start_cycle;
    int saw_final = 0;

    if (hartid == 0) {
        for (index = 1; index <= LAB_SEQ_WRITES; ++index) {
            lab_seq_write_begin(&seq_data);
            seq_data.x = index;
            hold_critical_window();
            seq_data.y = ~index;
            lab_seq_write_end(&seq_data);
        }
    } else if (hartid == 1) {
        start_cycle = read_cycle();
        do {
            start_version = lab_seq_read_begin(&seq_data);
            x = seq_data.x;
            y = seq_data.y;
            if (!lab_seq_read_retry(&seq_data, start_version)) {
                if (y != ~x) {
                    infra_or_failure(UINT64_C(1) << 2);
                }
                if (x == LAB_SEQ_WRITES) {
                    saw_final = 1;
                }
            }
        } while (!saw_final &&
                 read_cycle() - start_cycle <= STRESS_TIMEOUT_CYCLES);
        if (!saw_final) {
            infra_or_failure(UINT64_C(1) << 2);
        }
    }
    infra_barrier(local_sense);
    if (hartid == 0 &&
        (seq_data.sequence != 2 * LAB_SEQ_WRITES ||
         seq_data.x != LAB_SEQ_WRITES ||
         seq_data.y != ~UINT64_C(LAB_SEQ_WRITES))) {
        infra_or_failure(UINT64_C(1) << 2);
    }
    infra_barrier(local_sense);
}

static void run_stack_stage(uint64_t hartid, uint64_t *local_sense)
{
    uint64_t index;
    uint64_t node_index;
    uint64_t bit;
    uint64_t old;
    uintptr_t address;
    uintptr_t base = (uintptr_t)&stack_nodes[0];
    uintptr_t end = (uintptr_t)&stack_nodes[LAB_STACK_NODES];
    struct lab_stack_node *node;

    for (index = 0; index < LAB_STACK_PER_HART; ++index) {
        node_index = hartid * LAB_STACK_PER_HART + index;
        lab_stack_push(&stack, &stack_nodes[node_index]);
    }
    infra_barrier(local_sense);

    for (index = 0; index < LAB_STACK_PER_HART; ++index) {
        node = lab_stack_pop_protected(&stack, &hazard_slots[hartid]);
        address = (uintptr_t)node;
        if (address < base || address >= end ||
            (address - base) % sizeof(struct lab_stack_node) != 0) {
            infra_or_failure(UINT64_C(1) << 3);
        } else {
            node_index = (address - base) / sizeof(struct lab_stack_node);
            bit = UINT64_C(1) << node_index;
            old = infra_amoor(&popped_bitmap, bit);
            if ((old & bit) != 0 || node->value != node_index + 1) {
                infra_or_failure(UINT64_C(1) << 3);
            }
            if (hazard_slots[hartid] != address ||
                !lab_hazard_is_protected(hazard_slots, LAB_HART_COUNT, node)) {
                infra_or_failure(UINT64_C(1) << 3);
            }
        }
        lab_hazard_clear(&hazard_slots[hartid]);
        if (hazard_slots[hartid] != 0) {
            infra_or_failure(UINT64_C(1) << 3);
        }
    }
    infra_barrier(local_sense);
    if (hartid == 0 &&
        (popped_bitmap != UINT64_C(0xffffffff) || stack.head != 0)) {
        infra_or_failure(UINT64_C(1) << 3);
    }
    infra_barrier(local_sense);
}

static void run_qsbr_stage(uint64_t hartid, uint64_t *local_sense)
{
    lab_qsbr_report(&qsbr_domain, hartid);
    infra_barrier(local_sense);

    if (hartid == 0) {
        grace_target = lab_qsbr_start(&qsbr_domain);
    }
    infra_barrier(local_sense);
    if (hartid == 0 &&
        (grace_target != 2 ||
         lab_qsbr_complete(&qsbr_domain, grace_target) != 0)) {
        infra_or_failure(UINT64_C(1) << 4);
    }
    infra_barrier(local_sense);

    lab_qsbr_report(&qsbr_domain, hartid);
    infra_barrier(local_sense);
    if (hartid == 0 &&
        lab_qsbr_complete(&qsbr_domain, grace_target) != 1) {
        infra_or_failure(UINT64_C(1) << 4);
    }
    infra_barrier(local_sense);
}

void lab_stress_main(uint64_t hartid)
{
    uint64_t local_sense = 0;
    uint64_t index;
    unsigned int failures = 0;

    for (index = 0; index < LAB_STACK_PER_HART; ++index) {
        uint64_t node_index = hartid * LAB_STACK_PER_HART + index;
        stack_nodes[node_index].next = 0;
        stack_nodes[node_index].value = node_index + 1;
    }
    if (hartid == 0) {
        seq_data.y = ~UINT64_C(0);
        qsbr_domain.global_epoch = 1;
    }
    infra_barrier(&local_sense);

    run_mcs_stage(hartid, &local_sense);
    run_spsc_stage(hartid, &local_sense);
    run_seqlock_stage(hartid, &local_sense);
    run_stack_stage(hartid, &local_sense);
    run_qsbr_stage(hartid, &local_sense);

    if (hartid != 0) {
        for (;;) {
            __asm__ volatile ("wfi");
        }
    }

    lab_puts("LAB05 four-hart scalable synchronization stress\n");
    failures += lab_report("MCS local-spin critical section",
                           (failure_bits & (UINT64_C(1) << 0)) == 0);
    failures += lab_report("SPSC release/acquire FIFO",
                           (failure_bits & (UINT64_C(1) << 1)) == 0);
    failures += lab_report("seqlock consistent snapshot",
                           (failure_bits & (UINT64_C(1) << 2)) == 0);
    failures += lab_report("Treiber stack and hazard publication",
                           (failure_bits & (UINT64_C(1) << 3)) == 0);
    failures += lab_report("QSBR grace period completion",
                           (failure_bits & (UINT64_C(1) << 4)) == 0);
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB05 PASS\n" : "LAB05 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

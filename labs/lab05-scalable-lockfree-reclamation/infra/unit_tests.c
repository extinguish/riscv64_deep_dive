#include "lab_api.h"

_Static_assert(sizeof(struct lab_mcs_node) == LAB_MCS_NODE_SIZE,
               "MCS node layout does not match assembly offsets");
_Static_assert(sizeof(struct lab_spsc_ring) == LAB_RING_SIZE,
               "SPSC ring layout does not match assembly offsets");
_Static_assert(sizeof(struct lab_stack_node) == LAB_STACK_NODE_SIZE,
               "stack node layout does not match assembly offsets");
_Static_assert(sizeof(struct lab_qsbr) == LAB_QSBR_SIZE,
               "QSBR layout does not match assembly offsets");

static int test_mcs_contract(void)
{
    struct lab_mcs_lock lock = {0};
    struct lab_mcs_node node = {0};

    lab_mcs_lock(&lock, &node);
    if (lock.tail != (uintptr_t)&node || node.locked != 0 || node.next != 0) {
        return 0;
    }
    lab_mcs_unlock(&lock, &node);
    return lock.tail == 0;
}

static int test_spsc_contract(void)
{
    struct lab_spsc_ring ring = {0};
    uint64_t value = 0;
    uint64_t index;

    if (lab_spsc_try_pop(&ring, &value) != 0) {
        return 0;
    }
    for (index = 0; index < LAB_SPSC_CAPACITY; ++index) {
        if (lab_spsc_try_push(&ring, index + 0x10u) != 1) {
            return 0;
        }
    }
    if (lab_spsc_try_push(&ring, 0xfeedu) != 0) {
        return 0;
    }
    for (index = 0; index < LAB_SPSC_CAPACITY; ++index) {
        if (lab_spsc_try_pop(&ring, &value) != 1 ||
            value != index + 0x10u) {
            return 0;
        }
    }
    return lab_spsc_try_pop(&ring, &value) == 0 &&
           ring.head == LAB_SPSC_CAPACITY &&
           ring.tail == LAB_SPSC_CAPACITY;
}

static int test_seqlock_contract(void)
{
    struct lab_seq_data data = {0};
    uint64_t start;
    uint64_t x;
    uint64_t y;

    lab_seq_write_begin(&data);
    if (data.sequence != 1) {
        return 0;
    }
    data.x = UINT64_C(0x12345678);
    data.y = ~data.x;
    lab_seq_write_end(&data);
    if (data.sequence != 2) {
        return 0;
    }

    start = lab_seq_read_begin(&data);
    x = data.x;
    y = data.y;
    if (start != 2 || lab_seq_read_retry(&data, start) != 0 || y != ~x) {
        return 0;
    }

    lab_seq_write_begin(&data);
    data.x = 9;
    data.y = ~data.x;
    lab_seq_write_end(&data);
    return lab_seq_read_retry(&data, start) == 1 && data.sequence == 4;
}

static int test_stack_hazard_contract(void)
{
    struct lab_stack stack = {0};
    struct lab_stack_node first = {0};
    struct lab_stack_node second = {0};
    struct lab_stack_node *node;
    volatile uintptr_t hazards[LAB_HART_COUNT] = {0};

    first.value = 1;
    second.value = 2;
    lab_stack_push(&stack, &first);
    lab_stack_push(&stack, &second);

    node = lab_stack_pop_protected(&stack, &hazards[0]);
    if (node != &second || hazards[0] != (uintptr_t)&second ||
        !lab_hazard_is_protected(hazards, LAB_HART_COUNT, &second)) {
        return 0;
    }
    lab_hazard_clear(&hazards[0]);
    if (hazards[0] != 0 ||
        lab_hazard_is_protected(hazards, LAB_HART_COUNT, &second)) {
        return 0;
    }

    node = lab_stack_pop_protected(&stack, &hazards[0]);
    if (node != &first || hazards[0] != (uintptr_t)&first) {
        return 0;
    }
    lab_hazard_clear(&hazards[0]);
    return lab_stack_pop_protected(&stack, &hazards[0]) == 0 &&
           hazards[0] == 0 && stack.head == 0;
}

static int test_qsbr_contract(void)
{
    struct lab_qsbr domain = {0};
    uint64_t target;
    uint64_t hart;

    domain.global_epoch = 1;
    for (hart = 0; hart < LAB_HART_COUNT; ++hart) {
        lab_qsbr_report(&domain, hart);
    }
    target = lab_qsbr_start(&domain);
    if (target != 2 || domain.global_epoch != 2 ||
        lab_qsbr_complete(&domain, target) != 0) {
        return 0;
    }
    for (hart = 0; hart + 1 < LAB_HART_COUNT; ++hart) {
        lab_qsbr_report(&domain, hart);
    }
    if (lab_qsbr_complete(&domain, target) != 0) {
        return 0;
    }
    lab_qsbr_report(&domain, LAB_HART_COUNT - 1);
    return lab_qsbr_complete(&domain, target) == 1;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("MCS uncontended enqueue/unlock contract",
                           test_mcs_contract());
    failures += lab_report("SPSC full/empty and FIFO contract",
                           test_spsc_contract());
    failures += lab_report("seqlock version and retry contract",
                           test_seqlock_contract());
    failures += lab_report("Treiber stack hazard lifetime contract",
                           test_stack_hazard_contract());
    failures += lab_report("QSBR target epoch completion contract",
                           test_qsbr_contract());
    return failures;
}

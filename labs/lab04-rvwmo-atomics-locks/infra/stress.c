#include "lab_api.h"

static volatile uint64_t barrier_count;
static volatile uint64_t barrier_sense;
static volatile uint64_t failure_bits;
static volatile uint64_t atomic_counter;
static struct lab_spinlock spin_lock;
static volatile uint64_t spin_counter;
static volatile uint64_t spin_in_critical;
static struct lab_ticketlock ticket_lock;
static volatile uint64_t ticket_counter;
static volatile uint64_t ticket_in_critical;
static volatile uint64_t bit_word;
static struct lab_mailbox mailbox;

static uint64_t infra_amoadd(volatile uint64_t *address, uint64_t value)
{
    uint64_t old;
    __asm__ volatile ("amoadd.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(address), "r"(value) : "memory");
    return old;
}

static void infra_or_failure(uint64_t bits)
{
    __asm__ volatile ("amoor.d.aqrl zero, %1, (%0)"
                      : : "r"(&failure_bits), "r"(bits) : "memory");
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
    for (count = 0; count < 64; ++count) {
        __asm__ volatile ("nop");
    }
}

void lab_stress_main(uint64_t hartid)
{
    uint64_t local_sense = 0;
    uint64_t index;
    uint64_t old;
    uint64_t output = 0;
    uint64_t start_cycle;
    int received = 0;
    unsigned int failures = 0;

    for (index = 0; index < LAB_ATOMIC_ITERATIONS; ++index) {
        old = lab_atomic_fetch_add(&atomic_counter, 1);
        if (old >= LAB_HART_COUNT * LAB_ATOMIC_ITERATIONS) {
            infra_or_failure(UINT64_C(1) << 0);
        }
    }
    infra_barrier(&local_sense);
    if (hartid == 0 &&
        atomic_counter != LAB_HART_COUNT * LAB_ATOMIC_ITERATIONS) {
        infra_or_failure(UINT64_C(1) << 0);
    }
    infra_barrier(&local_sense);

    for (index = 0; index < LAB_LOCK_ITERATIONS; ++index) {
        lab_spin_lock(&spin_lock);
        if (spin_in_critical != 0) {
            infra_or_failure(UINT64_C(1) << 1);
        }
        spin_in_critical = 1;
        hold_critical_window();
        spin_counter = spin_counter + 1;
        spin_in_critical = 0;
        lab_spin_unlock(&spin_lock);
    }
    infra_barrier(&local_sense);
    if (hartid == 0 && spin_counter != LAB_HART_COUNT * LAB_LOCK_ITERATIONS) {
        infra_or_failure(UINT64_C(1) << 1);
    }
    infra_barrier(&local_sense);

    for (index = 0; index < LAB_LOCK_ITERATIONS; ++index) {
        lab_ticket_lock(&ticket_lock);
        if (ticket_in_critical != 0) {
            infra_or_failure(UINT64_C(1) << 2);
        }
        ticket_in_critical = 1;
        hold_critical_window();
        ticket_counter = ticket_counter + 1;
        ticket_in_critical = 0;
        lab_ticket_unlock(&ticket_lock);
    }
    infra_barrier(&local_sense);
    if (hartid == 0 && ticket_counter != LAB_HART_COUNT * LAB_LOCK_ITERATIONS) {
        infra_or_failure(UINT64_C(1) << 2);
    }
    infra_barrier(&local_sense);

    old = lab_atomic_test_and_set_bit(&bit_word, hartid);
    if (old != 0) {
        infra_or_failure(UINT64_C(1) << 3);
    }
    infra_barrier(&local_sense);
    old = lab_atomic_test_and_set_bit(&bit_word, hartid);
    if (old != 1) {
        infra_or_failure(UINT64_C(1) << 3);
    }
    infra_barrier(&local_sense);
    if (hartid == 0 && bit_word != 0xf) {
        infra_or_failure(UINT64_C(1) << 3);
    }
    infra_barrier(&local_sense);

    if (hartid == 0) {
        hold_critical_window();
        lab_publish_message(&mailbox, UINT64_C(LAB_MAILBOX_VALUE));
    } else if (hartid == 1) {
        __asm__ volatile ("rdcycle %0" : "=r"(start_cycle));
        do {
            received = lab_consume_message(&mailbox, &output);
            __asm__ volatile ("rdcycle %0" : "=r"(old));
        } while (!received && old - start_cycle < UINT64_C(10000000));
        if (!received || output != UINT64_C(LAB_MAILBOX_VALUE)) {
            infra_or_failure(UINT64_C(1) << 4);
        }
    }
    infra_barrier(&local_sense);

    if (hartid != 0) {
        for (;;) {
            __asm__ volatile ("wfi");
        }
    }

    lab_puts("LAB04 four-hart concurrency stress\n");
    failures += lab_report("LR/SC atomic fetch-add",
                           (failure_bits & (UINT64_C(1) << 0)) == 0);
    failures += lab_report("acquire/release spinlock critical section",
                           (failure_bits & (UINT64_C(1) << 1)) == 0);
    failures += lab_report("ticket lock serialization",
                           (failure_bits & (UINT64_C(1) << 2)) == 0);
    failures += lab_report("AMO test-and-set bit",
                           (failure_bits & (UINT64_C(1) << 3)) == 0);
    failures += lab_report("release/acquire mailbox",
                           (failure_bits & (UINT64_C(1) << 4)) == 0);
    failures += lab_run_unit_tests();
    lab_puts(failures == 0 ? "LAB04 PASS\n" : "LAB04 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}

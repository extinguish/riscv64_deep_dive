#include "lab_api.h"

static volatile uint64_t full_arrived;
static volatile uint64_t full_bad_mapping;
static volatile uint64_t dynamic_state;
static volatile uint64_t secondary_ready;
static volatile uint64_t ipi_done;
static volatile uint64_t stopped;
static volatile uint64_t bad_wait_result;
static volatile uint64_t bad_publish_result;
static volatile uint64_t unexpected_trap;
static volatile uint64_t timer_seen[LAB_HART_COUNT];
static volatile uint64_t ipi_interrupts[LAB_HART_COUNT];
static volatile uint64_t ipi_seen[LAB_HART_COUNT];
static volatile uint64_t ipi_mailboxes[LAB_HART_COUNT]
    __attribute__((aligned(64)));

static uint64_t atomic_add(volatile uint64_t *address, uint64_t value)
{
    uint64_t old;
    __asm__ volatile ("amoadd.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(address), "r"(value) : "memory");
    return old;
}

static void atomic_or(volatile uint64_t *address, uint64_t value)
{
    __asm__ volatile ("amoor.d.aqrl zero, %1, (%0)"
                      : : "r"(address), "r"(value) : "memory");
}

static uint64_t load_acquire(const volatile uint64_t *address)
{
    uint64_t value = *address;
    __asm__ volatile ("fence r, rw" ::: "memory");
    return value;
}

static void store_release(volatile uint64_t *address, uint64_t value)
{
    __asm__ volatile ("fence rw, w" ::: "memory");
    *address = value;
}

static int wait_for_value(const volatile uint64_t *address, uint64_t value)
{
    uint64_t start = lab_read_mtime();
    while (load_acquire(address) != value) {
        if (lab_read_mtime() - start >= LAB_WAIT_TICKS) return 0;
    }
    return 1;
}

static void enable_interrupts(uint64_t mie_bits)
{
    __asm__ volatile ("csrs mie, %0" : : "r"(mie_bits) : "memory");
    __asm__ volatile ("csrs mstatus, %0" : : "r"(LAB_MSTATUS_MIE) : "memory");
}

static void disable_interrupts(uint64_t mie_bits)
{
    __asm__ volatile ("csrc mie, %0" : : "r"(mie_bits) : "memory");
    __asm__ volatile ("csrc mstatus, %0" : : "r"(LAB_MSTATUS_MIE) : "memory");
}

void lab_trap_dispatch(uint64_t hartid, uint64_t mcause)
{
    uint64_t code = mcause & LAB_MCAUSE_CODE_MASK;
    if (hartid >= LAB_HART_COUNT || (mcause & LAB_MCAUSE_INTERRUPT) == 0) {
        unexpected_trap = mcause | 1;
        lab_exit(1);
    }
    if (code == LAB_MCAUSE_MSI) {
        uint64_t reasons;
        *lab_msip(hartid) = 0;
        __asm__ volatile ("fence iorw, iorw" ::: "memory");
        reasons = lab_ipi_consume(&ipi_mailboxes[hartid]);
        if (reasons == UINT64_MAX ||
            (reasons & ~UINT64_C(LAB_IPI_REASON_MASK)) != 0) {
            unexpected_trap = UINT64_MAX;
            lab_exit(1);
        }
        atomic_or(&ipi_seen[hartid], reasons);
        atomic_add(&ipi_interrupts[hartid], 1);
        return;
    }
    if (code == LAB_MCAUSE_MTI) {
        *lab_mtimecmp(hartid) = UINT64_MAX;
        __asm__ volatile ("fence iorw, iorw" ::: "memory");
        timer_seen[hartid] = 1;
        return;
    }
    unexpected_trap = mcause | 1;
    lab_exit(1);
}

static void run_secondary(uint64_t hartid)
{
    uint64_t state;
    uint64_t snapshot;

    do {
        state = load_acquire(&dynamic_state);
    } while (state == LAB_DYNAMIC_WAIT);
    if (state != LAB_DYNAMIC_RUN) {
        for (;;) __asm__ volatile ("wfi");
    }

    *lab_mtimecmp(hartid) = UINT64_MAX;
    *lab_msip(hartid) = 0;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
    enable_interrupts(LAB_MIE_MSIE);
    atomic_add(&secondary_ready, 1);

    snapshot = lab_wait_for_bits(&ipi_seen[hartid], LAB_IPI_WORK_MASK);
    if ((snapshot & LAB_IPI_WORK_MASK) != LAB_IPI_WORK_MASK)
        atomic_or(&bad_wait_result, UINT64_C(1) << hartid);
    atomic_add(&ipi_done, 1);

    snapshot = lab_wait_for_bits(&ipi_seen[hartid], LAB_IPI_PANIC_STOP);
    if ((snapshot & LAB_IPI_PANIC_STOP) == 0)
        atomic_or(&bad_wait_result, UINT64_C(1) << hartid);
    disable_interrupts(LAB_MIE_MSIE);
    atomic_add(&stopped, 1);
    for (;;) __asm__ volatile ("wfi");
}

static int send_reasons(uint64_t first, uint64_t second)
{
    uint64_t hartid;
    int ok = 1;
    for (hartid = 1; hartid < LAB_HART_COUNT; ++hartid) {
        uint64_t old_first = lab_ipi_publish(&ipi_mailboxes[hartid], first);
        uint64_t old_second = first;
        if (second != 0)
            old_second = lab_ipi_publish(&ipi_mailboxes[hartid], second);
        if (old_first != 0 || old_second != first) {
            atomic_or(&bad_publish_result, UINT64_C(1) << hartid);
            ok = 0;
        }
    }
    __asm__ volatile ("fence ow, ow" ::: "memory");
    for (hartid = 1; hartid < LAB_HART_COUNT; ++hartid)
        *lab_msip(hartid) = 1;
    return ok;
}

static int reasons_delivered(void)
{
    uint64_t hartid;
    for (hartid = 1; hartid < LAB_HART_COUNT; ++hartid) {
        if ((ipi_seen[hartid] & LAB_IPI_WORK_MASK) != LAB_IPI_WORK_MASK ||
            ipi_interrupts[hartid] == 0) return 0;
    }
    return bad_publish_result == 0 && bad_wait_result == 0 &&
           unexpected_trap == 0;
}

static int run_timer_test(void)
{
    uint64_t now = lab_read_mtime();
    uint64_t local = now + LAB_TIMER_DELTA_LOCAL;
    uint64_t remote = now + LAB_TIMER_DELTA_REMOTE;
    uint64_t selected = lab_timer_select(now, local, remote);
    uint64_t timeout_start;

    if (selected != remote || !lab_timer_program(lab_mtimecmp(0), selected))
        return 0;
    enable_interrupts(LAB_MIE_MTIE);
    timeout_start = lab_read_mtime();
    while (load_acquire(&timer_seen[0]) == 0) {
        if (lab_read_mtime() - timeout_start >= LAB_WAIT_TICKS) {
            disable_interrupts(LAB_MIE_MTIE);
            return 0;
        }
        __asm__ volatile ("wfi");
    }
    disable_interrupts(LAB_MIE_MTIE);
    return unexpected_trap == 0;
}

void lab_full_main(uint64_t hartid)
{
    uint64_t logical = lab_hartid_to_cpu(hartid, lab_platform_hartids,
                                         LAB_HART_COUNT);
    unsigned int failures;
    int arrival_ok;
    int ready_ok;
    int ipi_ok;
    int timer_ok;
    int stop_ok;

    if (logical != hartid)
        atomic_or(&full_bad_mapping, UINT64_C(1) << hartid);
    atomic_add(&full_arrived, 1);
    if (hartid != 0) run_secondary(hartid);

    lab_puts("LAB12 multi-hart IPI and timer tests\n");
    arrival_ok = wait_for_value(&full_arrived, LAB_HART_COUNT);
    failures = lab_run_unit_tests();
    failures += lab_report("all harts entered with runtime mapping",
                           arrival_ok && full_bad_mapping == 0);
    if (failures != 0) {
        store_release(&dynamic_state, LAB_DYNAMIC_ABORT);
        failures += lab_report("dynamic SMP phase enabled", 0);
        lab_puts("LAB12 INCOMPLETE\n");
        lab_exit(1);
    }

    store_release(&dynamic_state, LAB_DYNAMIC_RUN);
    ready_ok = wait_for_value(&secondary_ready, LAB_HART_COUNT - 1);
    ipi_ok = ready_ok && send_reasons(LAB_IPI_RESCHEDULE,
                                      LAB_IPI_CALL_FUNCTION) &&
             wait_for_value(&ipi_done, LAB_HART_COUNT - 1) &&
             reasons_delivered();
    failures += lab_report("coalesced reason bits wake three harts", ipi_ok);

    timer_ok = run_timer_test();
    failures += lab_report("absolute CLINT deadline raises MTIP", timer_ok);

    if (ready_ok) {
        (void)send_reasons(LAB_IPI_PANIC_STOP, 0);
        stop_ok = wait_for_value(&stopped, LAB_HART_COUNT - 1) &&
                  bad_wait_result == 0;
    } else {
        stop_ok = 0;
    }
    failures += lab_report("panic-stop reason parks secondaries", stop_ok);
    lab_puts(failures ? "LAB12 INCOMPLETE\n" : "LAB12 PASS\n");
    lab_exit(failures ? 1u : 0u);
}

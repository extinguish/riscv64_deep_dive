#include "lab_api.h"

static volatile uint64_t arrived;
static volatile uint64_t seen_harts;
static volatile uint64_t owner_roles;
static volatile uint64_t bad_mapping;
static volatile uint64_t bad_stack;
static volatile uint64_t bad_gp;
static volatile uint64_t bad_tp;
static volatile uint64_t bad_fdt;
static volatile uint64_t bad_owner;
static volatile uint64_t bad_sections;
static volatile uint64_t bad_publication;

static uint64_t atomic_fetch_add(volatile uint64_t *address, uint64_t value)
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

static int region_matches(const unsigned char *left,
                          const unsigned char *right, size_t size)
{
    size_t index;
    for (index = 0; index < size; ++index) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static int region_is_zero(const unsigned char *start, size_t size)
{
    size_t index;
    for (index = 0; index < size; ++index) {
        if (start[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static int valid_qemu_fdt(uintptr_t fdt)
{
    const uint8_t *bytes;
    if ((fdt & 7u) != 0u || fdt < UINT64_C(0x80000000) ||
        fdt >= UINT64_C(0x88000000)) {
        return 0;
    }
    bytes = (const uint8_t *)fdt;
    return bytes[0] == 0xd0u && bytes[1] == 0x0du &&
           bytes[2] == 0xfeu && bytes[3] == 0xedu;
}

void lab_boot_hart_main_c(uint64_t hartid, uintptr_t fdt,
                          uint64_t logical_index, uint64_t is_owner,
                          uintptr_t stack_pointer, uintptr_t global_pointer,
                          uintptr_t thread_pointer)
{
    uint64_t hart_bit = hartid < 64 ? UINT64_C(1) << hartid : 0;
    uintptr_t expected_stack_start;
    uintptr_t expected_stack_end;
    uintptr_t expected_tp;
    size_t data_size = (size_t)(__lab_data_end - __lab_data_start);
    size_t bss_size = (size_t)(__lab_bss_end - __lab_bss_start);
    unsigned int failures = 0;

    if (logical_index >= LAB_HART_COUNT ||
        lab_platform_hart_ids[logical_index] != hartid) {
        atomic_or(&bad_mapping, hart_bit != 0 ? hart_bit : 1);
        logical_index = 0;
    }
    atomic_or(&seen_harts, UINT64_C(1) << logical_index);

    expected_stack_start = (uintptr_t)__hart_stacks_start +
        logical_index * LAB_HART_STACK_SIZE;
    expected_stack_end = expected_stack_start + LAB_HART_STACK_SIZE;
    if (stack_pointer < expected_stack_start ||
        stack_pointer > expected_stack_end || (stack_pointer & 15u) != 0u) {
        atomic_or(&bad_stack, hart_bit);
    }

    if (global_pointer != (uintptr_t)__global_pointer$) {
        atomic_or(&bad_gp, hart_bit);
    }
    expected_tp = (uintptr_t)__per_hart_start +
        logical_index * LAB_PER_HART_SIZE;
    if (thread_pointer != expected_tp) {
        atomic_or(&bad_tp, hart_bit);
    }
    if (!valid_qemu_fdt(fdt)) {
        atomic_or(&bad_fdt, hart_bit);
    }
    if (is_owner > 1) {
        atomic_or(&bad_owner, hart_bit);
    } else if (is_owner == 1) {
        atomic_fetch_add(&owner_roles, 1);
        if (lab_boot_owner != hartid) {
            atomic_or(&bad_owner, hart_bit);
        }
    }

    if (!region_matches(__lab_data_start, __lab_data_load_start, data_size) ||
        !region_is_zero(__lab_bss_start, bss_size)) {
        atomic_or(&bad_sections, hart_bit);
    }
    if (lab_boot_ready != 1 ||
        lab_boot_publication != UINT64_C(LAB_BOOT_PUBLICATION_VALUE)) {
        atomic_or(&bad_publication, hart_bit);
    }

    atomic_fetch_add(&arrived, 1);

    if (hartid != 0) {
        for (;;) {
            __asm__ volatile ("wfi");
        }
    }

    while (load_acquire(&arrived) != LAB_HART_COUNT) {
    }

    lab_puts("LAB02 multi-hart boot tests\n");
    failures += lab_report("all four harts reached runtime",
                           arrived == LAB_HART_COUNT && seen_harts == 0xf);
    failures += lab_report("hartid to logical-index mapping", bad_mapping == 0);
    failures += lab_report("isolated aligned permanent stacks", bad_stack == 0);
    failures += lab_report("gp initialized to __global_pointer$", bad_gp == 0);
    failures += lab_report("tp points at per-hart storage", bad_tp == 0);
    failures += lab_report("FDT handoff preserved", bad_fdt == 0);
    failures += lab_report("exactly one coldboot owner",
                           owner_roles == 1 && bad_owner == 0 &&
                           lab_boot_owner != UINT64_MAX);
    failures += lab_report("data copied and lab BSS cleared", bad_sections == 0);
    failures += lab_report("boot state published before release",
                           bad_publication == 0);
    failures += lab_run_helper_tests();

    lab_puts(failures == 0 ? "LAB02 PASS\n" : "LAB02 INCOMPLETE\n");
    lab_exit(failures == 0 ? 0u : 1u);
}


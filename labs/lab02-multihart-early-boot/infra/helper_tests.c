#include "lab_api.h"

static int bytes_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    size_t index;
    for (index = 0; index < size; ++index) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t relocated_copy[512] __attribute__((aligned(16)));

unsigned int lab_run_helper_tests(void)
{
    static const uint64_t sparse_harts[] = {7, 42, 0x1000, 3};
    uint8_t source[20];
    uint8_t destination[20];
    uint8_t zero_area[20];
    const size_t reloc_size =
        (size_t)(__reloc_image_end - __reloc_image_start);
    const uintptr_t reloc_offset =
        (uintptr_t)lab_relocated_stage - (uintptr_t)__reloc_image_start;
    uint64_t (*relocated_function)(uint64_t);
    const uint64_t input = UINT64_C(0x1122334455667788);
    const uint64_t expected =
        (input ^ UINT64_C(LAB_RELOC_INTERNAL_VALUE)) + LAB_RELOC_ADDEND;
    unsigned int failures = 0;
    size_t index;
    int passed;

    passed = lab_hartid_to_index(7, sparse_harts, 4) == 0 &&
             lab_hartid_to_index(0x1000, sparse_harts, 4) == 2 &&
             lab_hartid_to_index(99, sparse_harts, 4) == -1 &&
             lab_hartid_to_index(7, sparse_harts, 0) == -1;
    failures += lab_report("sparse hartid to logical-index mapping", passed);

    for (index = 0; index < sizeof(source); ++index) {
        source[index] = (uint8_t)(index * 9u + 1u);
        destination[index] = 0xccu;
        zero_area[index] = 0xa5u;
    }
    lab_init_regions(&destination[3], &source[2], 13,
                     &zero_area[4], 11);
    passed = bytes_equal(&destination[3], &source[2], 13);
    for (index = 0; index < sizeof(destination); ++index) {
        if ((index < 3 || index >= 16) && destination[index] != 0xccu) {
            passed = 0;
        }
        if (zero_area[index] != ((index >= 4 && index < 15) ? 0u : 0xa5u)) {
            passed = 0;
        }
    }
    failures += lab_report("data copy and BSS clear with canaries", passed);

    passed = reloc_size != 0 && reloc_size <= sizeof(relocated_copy) &&
             reloc_offset < reloc_size;
    if (passed) {
        for (index = 0; index < reloc_size; ++index) {
            relocated_copy[index] = __reloc_image_start[index];
        }
        __asm__ volatile ("fence.i" ::: "memory");
        relocated_function = (uint64_t (*)(uint64_t))
            (uintptr_t)((uintptr_t)relocated_copy + reloc_offset);
        passed = lab_relocated_stage(input) == expected &&
                 relocated_function(input) == expected;
    }
    failures += lab_report("copied relocation image executes correctly", passed);
    return failures;
}


#include "lab_api.h"

const uint64_t lab_global_anchor = LAB_GLOBAL_VALUE;

static unsigned int tests_run;
static unsigned int tests_failed;

static void report(const char *name, int passed)
{
    ++tests_run;
    if (!passed) {
        ++tests_failed;
    }
    lab_puts(passed ? "[PASS] " : "[FAIL] ");
    lab_puts(name);
    lab_puts("\n");
}

static int bytes_equal(const uint8_t *lhs, const uint8_t *rhs, size_t size)
{
    size_t index;
    for (index = 0; index < size; ++index) {
        if (lhs[index] != rhs[index]) {
            return 0;
        }
    }
    return 1;
}

static void fill_bytes(uint8_t *buffer, size_t size, uint8_t value)
{
    size_t index;
    for (index = 0; index < size; ++index) {
        buffer[index] = value;
    }
}

static void test_memzero(void)
{
    uint8_t buffer[37];
    size_t index;
    int passed = 1;

    fill_bytes(buffer, sizeof(buffer), 0xa5u);
    asm_memzero(&buffer[3], 29);
    for (index = 0; index < sizeof(buffer); ++index) {
        uint8_t expected = (index >= 3 && index < 32) ? 0u : 0xa5u;
        if (buffer[index] != expected) {
            passed = 0;
        }
    }
    asm_memzero((void *)(uintptr_t)0, 0);
    report("asm_memzero: unaligned range, canary, zero size", passed);
}

static void test_memmove(void)
{
    uint8_t source[32];
    uint8_t destination[32];
    uint8_t forward[16] = {0, 1, 2, 3, 4, 5, 6, 7,
                           8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t backward[16] = {0, 1, 2, 3, 4, 5, 6, 7,
                            8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t forward_expected[16] =
        {4, 5, 6, 7, 8, 9, 10, 11, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t backward_expected[16] =
        {0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
    size_t index;
    int passed = 1;

    for (index = 0; index < sizeof(source); ++index) {
        source[index] = (uint8_t)(index * 7u + 3u);
        destination[index] = 0xa5u;
    }
    if (asm_memmove(&destination[3], &source[1], 23) != &destination[3]) {
        passed = 0;
    }
    for (index = 0; index < sizeof(destination); ++index) {
        uint8_t expected = (index >= 3 && index < 26)
            ? source[1 + index - 3] : 0xa5u;
        if (destination[index] != expected) {
            passed = 0;
        }
    }
    asm_memmove(&forward[0], &forward[4], 8);
    asm_memmove(&backward[4], &backward[0], 8);
    if (!bytes_equal(forward, forward_expected, sizeof(forward)) ||
        !bytes_equal(backward, backward_expected, sizeof(backward))) {
        passed = 0;
    }
    report("asm_memmove: copy and both overlap directions", passed);
}

static void test_add128(void)
{
    const uint64_t lhs[2] = {UINT64_MAX, UINT64_C(0x123456789abcdef0)};
    const uint64_t rhs[2] = {1, UINT64_C(0x1111111111111111)};
    const uint64_t expected[2] = {0, UINT64_C(0x23456789abcdf002)};
    const uint64_t max_value[2] = {UINT64_MAX, UINT64_MAX};
    const uint64_t one[2] = {1, 0};
    uint64_t output[2] = {UINT64_C(0xdeadbeef), UINT64_C(0xfeedface)};
    uint64_t wrapped[2] = {UINT64_C(0xdeadbeef), UINT64_C(0xfeedface)};

    asm_add128(output, lhs, rhs);
    asm_add128(wrapped, max_value, one);
    report("asm_add128: low-limb carry propagation",
           output[0] == expected[0] && output[1] == expected[1] &&
           wrapped[0] == 0 && wrapped[1] == 0);
}

static volatile uint64_t checksum_calls;

uint64_t lab_checksum_step(uint64_t accumulator, uint64_t value)
{
    ++checksum_calls;
    accumulator ^= value;
    accumulator = (accumulator << 9) | (accumulator >> (64 - 9));
    return accumulator + UINT64_C(0x9e3779b97f4a7c15);
}

static void test_abi_frame(void)
{
    const uint64_t values[] = {1, UINT64_C(0x0123456789abcdef),
                               UINT64_C(0xfedcba9876543210), 7};
    uint64_t expected = LAB_CHECKSUM_INIT;
    uint64_t actual = 0;
    uint64_t damaged;
    size_t index;

    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        expected = lab_checksum_step(expected, values[index]);
    }
    checksum_calls = 0;
    damaged = lab_check_callee_saved(asm_call_checksum, values,
                                     sizeof(values) / sizeof(values[0]), &actual);
    report("asm_call_checksum: functional result", actual == expected);
    report("asm_call_checksum: calls helper once per element",
           checksum_calls == sizeof(values) / sizeof(values[0]));
    if (damaged != 0) {
        lab_puts("  damaged s-register mask: ");
        lab_puthex(damaged);
        lab_puts("\n");
    }
    report("asm_call_checksum: preserves s0-s11", damaged == 0);
}

static void test_addressing(void)
{
    report("asm_global_address: PC-relative address",
           asm_global_address() == (uintptr_t)&lab_global_anchor);
    report("asm_global_load: PC-relative data load",
           asm_global_load() == lab_global_anchor);
}

static uint8_t blob_copy[256] __attribute__((aligned(16)));

static void test_pic_blob(void)
{
    size_t blob_size = (size_t)(__lab_blob_end - __lab_blob_start);
    uintptr_t offset = (uintptr_t)asm_pic_blob - (uintptr_t)__lab_blob_start;
    uint64_t (*copied_function)(uint64_t);
    size_t index;
    uint64_t original;
    uint64_t copied;
    int layout_ok = blob_size != 0 && blob_size <= sizeof(blob_copy) &&
                    offset < blob_size;

    original = asm_pic_blob(11);
    if (!layout_ok) {
        report("asm_pic_blob: linker boundaries", 0);
        report("asm_pic_blob: copied execution", 0);
        return;
    }
    for (index = 0; index < blob_size; ++index) {
        blob_copy[index] = __lab_blob_start[index];
    }
    __asm__ volatile ("fence.i" ::: "memory");
    copied_function = (uint64_t (*)(uint64_t))
        (uintptr_t)((uintptr_t)blob_copy + offset);
    copied = copied_function(11);

    report("asm_pic_blob: original execution", original == 40);
    report("asm_pic_blob: copied execution", copied == 40);
}

int main(void)
{
    lab_puts("LAB01 learner test suite\n");
    test_memzero();
    test_memmove();
    test_add128();
    test_abi_frame();
    test_addressing();
    test_pic_blob();

    lab_puts("Tests run: ");
    lab_puthex(tests_run);
    lab_puts(", failed: ");
    lab_puthex(tests_failed);
    lab_puts("\n");
    lab_puts(tests_failed == 0 ? "LAB01 PASS\n" : "LAB01 INCOMPLETE\n");
    return tests_failed == 0 ? 0 : 1;
}

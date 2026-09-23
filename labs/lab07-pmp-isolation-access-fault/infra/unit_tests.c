#include "lab_api.h"

static uint64_t infra_napot(uint64_t base, uint64_t size)
{
    return (base >> 2) | ((size >> 3) - 1);
}

static int test_napot_encoding(void)
{
    const uint64_t base = UINT64_C(0x80004000);
    const uint64_t size = LAB_PAGE_SIZE;
    const uint64_t encoded = infra_napot(base, size);
    uint64_t decoded_base = 0;
    uint64_t decoded_size = 0;

    if (lab_pmp_napot_encode(base, size) != encoded ||
        !lab_pmp_napot_decode(encoded, &decoded_base, &decoded_size) ||
        decoded_base != base || decoded_size != size) {
        return 0;
    }
    if (lab_pmp_napot_encode(base, 8) != (base >> 2) ||
        !lab_pmp_napot_decode(base >> 2, &decoded_base, &decoded_size) ||
        decoded_base != base || decoded_size != 8) {
        return 0;
    }
    return lab_pmp_napot_encode(base + 8, size) == UINT64_MAX &&
           lab_pmp_napot_encode(base, 12) == UINT64_MAX &&
           lab_pmp_napot_encode(base, 4) == UINT64_MAX;
}

static int test_tor_and_config(void)
{
    return lab_pmp_tor_encode(UINT64_C(0x80001000)) ==
               UINT64_C(0x20000400) &&
           lab_pmp_tor_encode(UINT64_C(0x80001002)) == UINT64_MAX &&
           lab_pmp_make_cfg(LAB_PMP_R | LAB_PMP_W,
                            LAB_PMP_A_NAPOT, 0) == UINT64_C(0x1b) &&
           lab_pmp_make_cfg(LAB_PMP_R, LAB_PMP_A_TOR, 1) ==
               UINT64_C(0x89) &&
           lab_pmp_make_cfg(LAB_PMP_W, LAB_PMP_A_NAPOT, 0) == UINT64_MAX &&
           lab_pmp_make_cfg(LAB_PMP_R, 4, 0) == UINT64_MAX &&
           lab_pmp_make_cfg(LAB_PMP_R, LAB_PMP_A_NAPOT, 2) == UINT64_MAX &&
           lab_pmp_make_cfg(LAB_PMP_R, LAB_PMP_A_OFF, 0) == UINT64_MAX;
}

static int test_priority_matcher(void)
{
    const uint64_t base = UINT64_C(0x80004000);
    uint64_t addresses[2];
    uint64_t config;

    addresses[0] = infra_napot(base, LAB_PAGE_SIZE);
    addresses[1] = infra_napot(UINT64_C(0x80000000),
                               UINT64_C(0x10000000));

    config = UINT64_C(0x18) | (UINT64_C(0x1f) << 8);
    if (lab_pmp_match(addresses, config, 2, base, 8, LAB_PMP_R) != 0 ||
        lab_pmp_match(addresses, config, 2, base + LAB_PAGE_SIZE + 8,
                      8, LAB_PMP_R) != 1 ||
        lab_pmp_match(addresses, config, 2, base - 4, 8, LAB_PMP_R) != 0) {
        return 0;
    }

    config = UINT64_C(0x19) | (UINT64_C(0x1f) << 8);
    if (lab_pmp_match(addresses, config, 2, base, 8, LAB_PMP_R) != 1 ||
        lab_pmp_match(addresses, config, 2, base, 8, LAB_PMP_W) != 0) {
        return 0;
    }

    config = UINT64_C(0x00) | (UINT64_C(0x1f) << 8);
    return lab_pmp_match(addresses, config, 2, base, 8, LAB_PMP_W) == 1 &&
           lab_pmp_match(addresses, config, 2, base, 0, LAB_PMP_R) == 0 &&
           lab_pmp_match(addresses, config, 2, UINT64_MAX - 3, 8,
                         LAB_PMP_R) == 0;
}

unsigned int lab_run_unit_tests(void)
{
    unsigned int failures = 0;

    failures += lab_report("NAPOT encode/decode and alignment",
                           test_napot_encoding());
    failures += lab_report("TOR and config-byte validation",
                           test_tor_and_config());
    failures += lab_report("PMP priority, overlap, and default policy",
                           test_priority_matcher());
    return failures;
}

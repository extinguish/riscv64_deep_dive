typedef unsigned char u8;
typedef unsigned long usize;

extern void *asm_memcpy(void *dst, const void *src, usize count);

static int bytes_equal(const u8 *left, const u8 *right, usize count)
{
    for (usize i = 0; i < count; ++i) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    static const u8 source[] = {
        0x00, 0xff, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
        0xde, 0xf0, 0x41, 0x42, 0x43, 0x7f, 0x80, 0x55,
    };
    static const u8 expected[] = {
        0x00, 0xff, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
        0xde, 0xf0, 0x41, 0x42, 0x43, 0x7f, 0x80, 0x55,
    };
    u8 destination[sizeof(source)];
    u8 untouched = 0xa5;

    for (usize i = 0; i < sizeof(destination); ++i) {
        destination[i] = 0;
    }

    if (asm_memcpy(destination, source, sizeof(source)) != destination) {
        return 1;
    }
    if (!bytes_equal(destination, expected, sizeof(expected))) {
        return 2;
    }

    /* A zero-byte copy must return dst and leave the destination unchanged. */
    if (asm_memcpy(&untouched, source, 0) != &untouched || untouched != 0xa5) {
        return 3;
    }

    return 0;
}


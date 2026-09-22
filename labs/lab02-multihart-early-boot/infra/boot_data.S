#include "lab_layout.h"

.section .rodata.platform, "a", @progbits
.align 3
.globl lab_platform_hart_ids
lab_platform_hart_ids:
    .dword 0, 1, 2, 3

.section .lab_data_load, "a", @progbits
.align 3
    .dword 0x0123456789abcdef
    .dword 0xfedcba9876543210
    .dword 0x1122334455667788
    .dword 0x8877665544332211

.section .boot_state, "aw", @progbits
.align 6
.globl lab_boot_owner
lab_boot_owner:
    .dword -1
.globl lab_boot_ready
lab_boot_ready:
    .dword 0
.globl lab_boot_publication
lab_boot_publication:
    .dword 0
.globl lab_smoke_arrived
lab_smoke_arrived:
    .dword 0
.globl lab_smoke_seen
lab_smoke_seen:
    .dword 0

.section .lab_data, "aw", @progbits
.align 3
    .dword 0xcccccccccccccccc
    .dword 0xcccccccccccccccc
    .dword 0xcccccccccccccccc
    .dword 0xcccccccccccccccc

.section .lab_bss, "aw", @progbits
.align 4
    .fill 128, 1, 0xa5


.section .boot_state, "aw", @progbits
.align 6
.globl lab_boot_ready
lab_boot_ready:
    .dword 0
.globl lab_smoke_arrived
lab_smoke_arrived:
    .dword 0

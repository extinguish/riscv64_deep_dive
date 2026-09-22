OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : ALIGN(16) {
        KEEP(*(.text.init))
        KEEP(*(.text.boot_runtime))
        KEEP(*(.text.boot.exercise))
        KEEP(*(.text.exercise))
        *(.text .text.*)
    } > RAM :text

    .reloc_image : ALIGN(16) {
        __reloc_image_start = .;
        KEEP(*(.reloc_image))
        __reloc_image_end = .;
    } > RAM :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } > RAM :rodata

    .lab_data_load : ALIGN(16) {
        __lab_data_load_start = .;
        KEEP(*(.lab_data_load))
        __lab_data_load_end = .;
    } > RAM :rodata

    .eh_frame : ALIGN(8) {
        KEEP(*(.eh_frame))
    } > RAM :rodata

    .data : ALIGN(16) {
        *(.data .data.*)
    } > RAM :data

    .sdata : ALIGN(16) {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    } > RAM :data

    .boot_state : ALIGN(64) {
        KEEP(*(.boot_state))
    } > RAM :data

    .lab_data : ALIGN(16) {
        __lab_data_start = .;
        KEEP(*(.lab_data))
        __lab_data_end = .;
    } > RAM :data

    .lab_bss : ALIGN(16) {
        __lab_bss_start = .;
        KEEP(*(.lab_bss))
        __lab_bss_end = .;
    } > RAM :data

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM :data

    .per_hart (NOLOAD) : ALIGN(64) {
        __per_hart_start = .;
        . += 4 * 64;
        __per_hart_end = .;
    } > RAM :data

    .emergency_stacks (NOLOAD) : ALIGN(16) {
        __emergency_stacks_start = .;
        . += 4 * 512;
        __emergency_stacks_end = .;
    } > RAM :data

    .hart_stacks (NOLOAD) : ALIGN(4096) {
        __hart_stacks_start = .;
        . += 4 * 4096;
        __hart_stacks_end = .;
    } > RAM :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }

    ASSERT(__lab_data_end - __lab_data_start ==
           __lab_data_load_end - __lab_data_load_start,
           "lab data source/destination sizes differ")
    ASSERT(__reloc_image_end - __reloc_image_start <= 512,
           "relocation image exceeds copy buffer")
    ASSERT((__hart_stacks_start & 4095) == 0,
           "per-hart stacks are not page aligned")
}

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M }
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }

SECTIONS
{
    . = ORIGIN(RAM);
    .text : ALIGN(16) {
        KEEP(*(.text.init)) KEEP(*(.text.trap)) KEEP(*(.text.entry))
        KEEP(*(.text.exercise)) *(.text .text.*)
    } > RAM :text
    .rodata : ALIGN(16) { *(.rodata .rodata.*) } > RAM :rodata
    .eh_frame : ALIGN(8) { KEEP(*(.eh_frame)) } > RAM :rodata
    .data : ALIGN(16) { *(.data.bootstrap) *(.data .data.*) } > RAM :data
    .sdata : ALIGN(16) {
        __global_pointer$ = . + 0x800; *(.sdata .sdata.*)
    } > RAM :data
    .bss (NOLOAD) : ALIGN(64) {
        __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        . = ALIGN(64); __bss_end = .;
    } > RAM :data
    .per_hart (NOLOAD) : ALIGN(64) {
        __per_hart_start = .; . += 4 * 64; __per_hart_end = .;
    } > RAM :data
    .hart_stacks (NOLOAD) : ALIGN(4096) {
        __hart_stacks_start = .; . += 4 * 4096; __hart_stacks_end = .;
    } > RAM :data
    /DISCARD/ : { *(.comment) *(.note .note.*) }

    ASSERT((__per_hart_start & 63) == 0, "per-hart area is not cache-line aligned")
    ASSERT((__hart_stacks_start & 4095) == 0, "hart stacks are not page aligned")
}

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
        KEEP(*(.text.flow))
        KEEP(*(.text.trap_runtime))
        KEEP(*(.text.trap.exercise))
        KEEP(*(.text.exercise))
        *(.text .text.*)
    } > RAM :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
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

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM :data

    .m_stack (NOLOAD) : ALIGN(16) {
        __m_stack_bottom = .;
        . += 4096;
        __m_stack_top = .;
    } > RAM :data

    .s_trap_stack (NOLOAD) : ALIGN(16) {
        __s_trap_stack_bottom = .;
        . += 16384;
        __s_trap_stack_top = .;
    } > RAM :data

    .u_stack (NOLOAD) : ALIGN(16) {
        __u_stack_bottom = .;
        . += 8192;
        __u_stack_top = .;
    } > RAM :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }

    ASSERT((__m_stack_top & 15) == 0, "M stack alignment")
    ASSERT((__s_trap_stack_top & 15) == 0, "S stack alignment")
    ASSERT((__u_stack_top & 15) == 0, "U stack alignment")
}

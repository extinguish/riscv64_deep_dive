OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    /* QEMU virt loads a bare-metal kernel at this physical address. */
    . = 0x80000000;

    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 16K;
    __stack_top = .;
}


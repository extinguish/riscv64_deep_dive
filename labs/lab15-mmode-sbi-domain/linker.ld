OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
  fwtext PT_LOAD FLAGS(5);
  fwdata PT_LOAD FLAGS(6);
  stext PT_LOAD FLAGS(5);
  sdata PT_LOAD FLAGS(6);
}
SECTIONS {
  .fwtext 0x80000000 : { KEEP(*(.text.init)) KEEP(*(.text.trap)) *(.text .text.*) *(.rodata .rodata.*) *(.eh_frame) } :fwtext
  ASSERT(. <= 0x80010000, "M text exceeds reserved region")
  .fwdata 0x80010000 : { *(.data .data.*) __bss_start = .; *(.bss .bss.*) *(COMMON)
              . = ALIGN(16); . += 16384; __m_stack_top = .;
              __bss_end = .; } :fwdata
  __global_pointer$ = ADDR(.fwdata) + 0x800;
  ASSERT(. <= 0x80200000, "M firmware exceeds protected region")
  .stext 0x80200000 : { KEEP(*(.payload_text)) } :stext
  ASSERT(. <= 0x80210000, "S text exceeds reserved region")
  .sdata 0x80210000 : { *(.payload_data) *(.payload_bss)
             . = ALIGN(16); . += 8192; __s_stack_top = .; } :sdata
  ASSERT(. <= 0x80400000, "S payload exceeds PMP region")
  /DISCARD/ : { *(.comment) *(.note .note.*) }
}

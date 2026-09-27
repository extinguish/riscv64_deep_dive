OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M }
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = ORIGIN(RAM);
  .text : ALIGN(16) { KEEP(*(.text.init)) KEEP(*(.text.trap)) KEEP(*(.text.guest)) *(.text .text.*) } > RAM :text
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } > RAM :rodata
  .eh_frame : ALIGN(8) { *(.eh_frame) } > RAM :rodata
  .data : ALIGN(16) { *(.data .data.*) } > RAM :data
  __global_pointer$ = . + 0x800;
  .bss (NOLOAD) : ALIGN(16384) {
    __bss_start = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16384); __bss_end = .;
  } > RAM :data
  .guest_page (NOLOAD) : ALIGN(2097152) {
    __guest_page = .; . += 4096;
  } > RAM :data
  .stack (NOLOAD) : ALIGN(16) {
    . += 16384; __stack_top = .;
    . += 4096; __guest_stack_top = .;
  } > RAM :data
  /DISCARD/ : { *(.comment) *(.note .note.*) }
  ASSERT((__guest_page & 2097151) == 0, "guest backing page needs 2 MiB alignment")
}

.section .text.entry
.align 2
.globl lab_hart_entry
.type lab_hart_entry, @function
lab_hart_entry:
    tail lab_unit_main
.size lab_hart_entry, .-lab_hart_entry

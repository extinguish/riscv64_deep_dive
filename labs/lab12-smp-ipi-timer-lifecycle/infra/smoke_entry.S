.section .text.entry
.align 2
.globl lab_hart_entry
.type lab_hart_entry, @function
lab_hart_entry:
    mv a1, sp
    mv a2, tp
    tail lab_smoke_main
.size lab_hart_entry, .-lab_hart_entry

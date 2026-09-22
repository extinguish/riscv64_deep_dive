.section .text.boot_runtime
.align 2
.globl lab_boot_hart_main
.type lab_boot_hart_main, @function
lab_boot_hart_main:
    mv      a4, sp
    mv      a5, gp
    mv      a6, tp
    tail    lab_boot_hart_main_c
.size lab_boot_hart_main, .-lab_boot_hart_main


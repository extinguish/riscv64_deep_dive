.section .text.flow
.align 2
.globl lab_s_kernel_entry
.type lab_s_kernel_entry, @function
lab_s_kernel_entry:
    lla     sp, __s_trap_stack_top
    tail    lab_unit_main
.size lab_s_kernel_entry, .-lab_s_kernel_entry

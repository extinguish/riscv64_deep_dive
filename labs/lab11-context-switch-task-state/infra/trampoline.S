.section .text.flow
.align 2
.globl lab_task_trampoline
.type lab_task_trampoline, @function
lab_task_trampoline:
    mv      a0, s1
    jalr    ra, s0, 0
    jalr    zero, s2, 0
.size lab_task_trampoline, .-lab_task_trampoline

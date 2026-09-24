.section .text.vector_example
.balign 128
.option push
.option norvc
.option norelax
.globl lab_vector_example
.type lab_vector_example, @function
lab_vector_example:
    .rept 16
    jal     zero, lab_vector_example_stub
    .endr
.globl lab_vector_example_end
lab_vector_example_end:
.size lab_vector_example, .-lab_vector_example

.type lab_vector_example_stub, @function
lab_vector_example_stub:
    ret
.size lab_vector_example_stub, .-lab_vector_example_stub
.option pop

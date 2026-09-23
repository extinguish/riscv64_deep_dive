#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3a: void lab_seq_write_begin(data) */
.globl lab_seq_write_begin
.type lab_seq_write_begin, @function
lab_seq_write_begin:
    ret
.size lab_seq_write_begin, .-lab_seq_write_begin

/* TODO 3b: void lab_seq_write_end(data) */
.globl lab_seq_write_end
.type lab_seq_write_end, @function
lab_seq_write_end:
    ret
.size lab_seq_write_end, .-lab_seq_write_end

/* TODO 3c: uint64_t lab_seq_read_begin(data) */
.globl lab_seq_read_begin
.type lab_seq_read_begin, @function
lab_seq_read_begin:
    li      a0, -1
    ret
.size lab_seq_read_begin, .-lab_seq_read_begin

/* TODO 3d: int lab_seq_read_retry(data, start) */
.globl lab_seq_read_retry
.type lab_seq_read_retry, @function
lab_seq_read_retry:
    li      a0, 1
    ret
.size lab_seq_read_retry, .-lab_seq_read_retry

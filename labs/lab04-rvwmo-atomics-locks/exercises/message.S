#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4a: void lab_publish_message(mailbox, value) */
.globl lab_publish_message
.type lab_publish_message, @function
lab_publish_message:
    ret
.size lab_publish_message, .-lab_publish_message

/* TODO 4b: int lab_consume_message(mailbox, output) */
.globl lab_consume_message
.type lab_consume_message, @function
lab_consume_message:
    li      a0, 0
    ret
.size lab_consume_message, .-lab_consume_message

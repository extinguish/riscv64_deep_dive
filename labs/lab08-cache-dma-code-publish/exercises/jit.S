#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 4: copy instruction words and publish them to this hart with fence.i. */
.globl lab_publish_code
.type lab_publish_code, @function
lab_publish_code:
    li      a0, 0
    ret
.size lab_publish_code, .-lab_publish_code

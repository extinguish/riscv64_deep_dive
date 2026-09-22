#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3a: void lab_ticket_lock(struct lab_ticketlock *lock) */
.globl lab_ticket_lock
.type lab_ticket_lock, @function
lab_ticket_lock:
    ret
.size lab_ticket_lock, .-lab_ticket_lock

/* TODO 3b: void lab_ticket_unlock(struct lab_ticketlock *lock) */
.globl lab_ticket_unlock
.type lab_ticket_unlock, @function
lab_ticket_unlock:
    ret
.size lab_ticket_unlock, .-lab_ticket_unlock

#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 2a: int lab_spin_trylock(struct lab_spinlock *lock) */
.globl lab_spin_trylock
.type lab_spin_trylock, @function
lab_spin_trylock:
    li      a0, 0
    ret
.size lab_spin_trylock, .-lab_spin_trylock

/* TODO 2b: void lab_spin_lock(struct lab_spinlock *lock) */
.globl lab_spin_lock
.type lab_spin_lock, @function
lab_spin_lock:
    ret
.size lab_spin_lock, .-lab_spin_lock

/* TODO 2c: void lab_spin_unlock(struct lab_spinlock *lock) */
.globl lab_spin_unlock
.type lab_spin_unlock, @function
lab_spin_unlock:
    ret
.size lab_spin_unlock, .-lab_spin_unlock

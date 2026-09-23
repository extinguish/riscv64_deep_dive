#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3a: publish a CPU-produced buffer and descriptor to a DMA device. */
.globl lab_dma_prepare_tx
.type lab_dma_prepare_tx, @function
lab_dma_prepare_tx:
    li      a0, 0
    ret
.size lab_dma_prepare_tx, .-lab_dma_prepare_tx

/* TODO 3b: acquire a device-produced buffer for the CPU. */
.globl lab_dma_complete_rx
.type lab_dma_complete_rx, @function
lab_dma_complete_rx:
    li      a0, 0
    ret
.size lab_dma_complete_rx, .-lab_dma_complete_rx

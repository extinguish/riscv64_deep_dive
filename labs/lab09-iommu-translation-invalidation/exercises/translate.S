#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 2: select a device/process context and perform enabled stages. */
.globl lab_iommu_translate
.type lab_iommu_translate, @function
lab_iommu_translate:
    li      a0, 0
    ret
.size lab_iommu_translate, .-lab_iommu_translate

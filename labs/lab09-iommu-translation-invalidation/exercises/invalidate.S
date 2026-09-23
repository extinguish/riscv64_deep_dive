#include "lab_layout.h"

.section .text.exercise
.align 2

/* TODO 3a: install a mapping, invalidate IOTLB, and await completion. */
.globl lab_iommu_map
.type lab_iommu_map, @function
lab_iommu_map:
    li      a0, 0
    ret
.size lab_iommu_map, .-lab_iommu_map

/* TODO 3b: clear a mapping and gate page reclamation on completion. */
.globl lab_iommu_unmap
.type lab_iommu_unmap, @function
lab_iommu_unmap:
    li      a0, 0
    ret
.size lab_iommu_unmap, .-lab_iommu_unmap

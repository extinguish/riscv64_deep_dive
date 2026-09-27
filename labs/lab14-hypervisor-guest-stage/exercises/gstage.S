.section .text.exercise
.align 2
.globl lab_gstage_index
lab_gstage_index:
    # TODO: validate the 41-bit GPA and return Sv39x4 root index [40:30].
    li a0, -1
    ret

.globl lab_gstage_leaf
lab_gstage_leaf:
    # TODO: validate 2 MiB PA alignment and legal R/W/X/U/A/D flags.
    # Return a valid G-stage level-1 leaf PTE; zero denotes invalid input.
    li a0, 0
    ret

.globl lab_guest_fault
lab_guest_fault:
    # TODO: return 1 for alias Load, 2 for MMIO Store, 3 for read-only
    # alias Store. Check SPV, scause, htval and stval; otherwise return 0.
    li a0, 0
    ret

.globl lab_install_mapping
lab_install_mapping:
    # TODO: validate slot/PTE/GPA, store PTE, order the store, then execute
    # HFENCE.GVMA with rs1 = GPA >> 2 and rs2 = x0. Return old PTE.
    li a0, -1
    ret

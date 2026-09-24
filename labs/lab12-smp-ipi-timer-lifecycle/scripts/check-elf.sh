#!/usr/bin/env bash
set -euo pipefail
elf=${1:?usage: check-elf.sh ELF}; readelf_bin=${READELF:-readelf}; nm_bin=${NM:-nm}
header=$($readelf_bin -h "$elf"); sections=$($readelf_bin -SW "$elf")
segments=$($readelf_bin -lW "$elf"); attributes=$($readelf_bin -A "$elf")
undefined=$($nm_bin -u "$elf" || true)
require_text() { if ! grep -Eq "$2" <<<"$1"; then echo "ELF CHECK FAIL: $3" >&2; exit 1; fi; }
require_text "$header" 'Class:[[:space:]]+ELF64' 'not an ELF64 image'
require_text "$header" 'Machine:[[:space:]]+RISC-V' 'not a RISC-V image'
require_text "$header" 'Entry point address:[[:space:]]+0x80000000' 'unexpected entry address'
require_text "$attributes" 'Tag_RISCV_arch:' 'missing RISC-V attributes'
for section in text bss per_hart hart_stacks; do
    require_text "$sections" "[[:space:]]\\.${section}[[:space:]]" "missing .$section section"
done
if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then echo 'ELF CHECK FAIL: writable executable segment' >&2; exit 1; fi
if [[ -n "$undefined" ]]; then echo "ELF CHECK FAIL: undefined symbols: $undefined" >&2; exit 1; fi
for symbol in _start lab_trap_entry lab_hartid_to_cpu lab_hart_mask_chunk \
  lab_cpu_transition lab_ipi_publish lab_ipi_consume lab_wait_for_bits \
  lab_timer_select lab_timer_program lab_full_main; do
    $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$" || { echo "ELF CHECK FAIL: missing $symbol" >&2; exit 1; }
done
echo 'ELF CHECK PASS: Lab 12 SMP sections and required symbols are present'

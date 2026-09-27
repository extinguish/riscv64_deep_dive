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
for section in text bss stack; do
    require_text "$sections" "[[:space:]]\\.${section}[[:space:]]" "missing .$section section"
done
if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then echo 'ELF CHECK FAIL: writable executable segment' >&2; exit 1; fi
if [[ -n "$undefined" ]]; then echo "ELF CHECK FAIL: undefined symbols: $undefined" >&2; exit 1; fi
for symbol in _start lab_trap_entry lab_plic_context_lookup lab_plic_select \
  lab_plic_service lab_plic_retarget lab_aplic_direct_target \
  lab_aplic_msi_target lab_imsic_guest_allocate lab_indirect_window_access \
  lab_full_main; do
    $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$" || { echo "ELF CHECK FAIL: missing $symbol" >&2; exit 1; }
done
echo 'ELF CHECK PASS: Lab 13 PLIC/AIA symbols and W^X layout are present'

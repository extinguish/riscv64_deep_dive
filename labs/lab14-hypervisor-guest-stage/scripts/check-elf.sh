#!/usr/bin/env bash
set -euo pipefail
elf=${1:?ELF required}
readelf_bin=${READELF:-readelf}
nm_bin=${NM:-nm}
objdump_bin=${OBJDUMP:-objdump}
header=$($readelf_bin -h "$elf")
segments=$($readelf_bin -lW "$elf")
attributes=$($readelf_bin -A "$elf")
[[ $header == *ELF64* && $header == *RISC-V* ]] || { echo 'ELF CHECK FAIL: expected RV64' >&2; exit 1; }
grep -q 'Tag_RISCV_arch:.*h' <<<"$attributes" || { echo 'ELF CHECK FAIL: missing H extension' >&2; exit 1; }
if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then echo 'ELF CHECK FAIL: W+X segment' >&2; exit 1; fi
for symbol in _start lab_hs_trap lab_guest_entry lab_gstage_index lab_gstage_leaf lab_guest_fault lab_install_mapping; do
  $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$" || { echo "ELF CHECK FAIL: missing $symbol" >&2; exit 1; }
done
mapping_code=$($objdump_bin -d "$elf" | awk '/<lab_install_mapping>:/ {inside=1; next} inside && /^$/ {exit} inside {print}')
grep -q 'hfence.gvma' <<<"$mapping_code" || {
  echo 'ELF CHECK FAIL: lab_install_mapping must execute HFENCE.GVMA' >&2; exit 1;
}
echo 'ELF CHECK PASS: RV64 H symbols and W^X segments'

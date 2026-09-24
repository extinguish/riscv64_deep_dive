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
for section in text bss task_stack stack; do
    require_text "$sections" "[[:space:]]\\.${section}[[:space:]]" "missing .$section section"
done
if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then echo 'ELF CHECK FAIL: writable executable segment' >&2; exit 1; fi
if [[ -n "$undefined" ]]; then echo "ELF CHECK FAIL: undefined symbols: $undefined" >&2; exit 1; fi
for symbol in _start lab_context_init lab_context_switch lab_vector_context_size \
  lab_extension_switch_plan lab_migration_allowed lab_task_trampoline \
  lab_task_probe lab_task_exit lab_full_main; do
    $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$" || { echo "ELF CHECK FAIL: missing $symbol" >&2; exit 1; }
done
echo 'ELF CHECK PASS: Lab 11 task stack and required symbols are present'

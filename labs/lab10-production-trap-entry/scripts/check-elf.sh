#!/usr/bin/env bash
set -euo pipefail

elf=${1:?usage: check-elf.sh ELF}
readelf_bin=${READELF:-readelf}
nm_bin=${NM:-nm}

header=$($readelf_bin -h "$elf")
sections=$($readelf_bin -SW "$elf")
segments=$($readelf_bin -lW "$elf")
attributes=$($readelf_bin -A "$elf")
undefined=$($nm_bin -u "$elf" || true)

require_text() {
    local text=$1
    local pattern=$2
    local description=$3
    if ! grep -Eq "$pattern" <<<"$text"; then
        echo "ELF CHECK FAIL: $description" >&2
        exit 1
    fi
}

require_text "$header" 'Class:[[:space:]]+ELF64' 'not an ELF64 image'
require_text "$header" 'Machine:[[:space:]]+RISC-V' 'not a RISC-V image'
require_text "$header" 'Entry point address:[[:space:]]+0x80000000' \
    'unexpected entry address'
require_text "$attributes" 'Tag_RISCV_arch:' 'missing RISC-V attributes'

for section in text bss trap_stack user_stack emergency_stack stack; do
    require_text "$sections" "[[:space:]]\\.${section}[[:space:]]" \
        "missing .$section section"
done

if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then
    echo "ELF CHECK FAIL: loadable segment is writable and executable" >&2
    exit 1
fi
if [[ -n "$undefined" ]]; then
    echo "ELF CHECK FAIL: undefined symbols remain:" >&2
    echo "$undefined" >&2
    exit 1
fi

for symbol in _start lab_status_on_trap lab_status_on_sret \
              lab_sanitize_user_sstatus lab_decode_scause lab_advance_epc \
              lab_s_trap_vector lab_s_trap_dispatch lab_enter_user \
              lab_vector_example lab_vector_example_end lab_m_trap_vector \
              lab_full_main; do
    if ! $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$"; then
        echo "ELF CHECK FAIL: missing symbol $symbol" >&2
        exit 1
    fi
done

vector_start_hex=$($nm_bin -n "$elf" | awk '$3 == "lab_vector_example" { print $1 }')
vector_end_hex=$($nm_bin -n "$elf" | awk '$3 == "lab_vector_example_end" { print $1 }')
if [[ -z "$vector_start_hex" || -z "$vector_end_hex" ]] ||
   (( 16#$vector_end_hex - 16#$vector_start_hex != 64 )); then
    echo "ELF CHECK FAIL: vectored example is not sixteen four-byte slots" >&2
    exit 1
fi

echo "ELF CHECK PASS: Lab 10 trap stacks, vector geometry, and symbols are present"

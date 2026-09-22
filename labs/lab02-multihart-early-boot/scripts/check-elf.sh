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

for section in reloc_image lab_data_load lab_data lab_bss boot_state hart_stacks; do
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

for symbol in _start lab_boot_entry lab_hartid_to_index lab_init_regions \
              lab_relocated_stage lab_boot_hart_main __hart_stacks_start \
              __per_hart_start __lab_data_start __lab_bss_start; do
    if ! $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$"; then
        echo "ELF CHECK FAIL: missing symbol $symbol" >&2
        exit 1
    fi
done

echo "ELF CHECK PASS: Lab 02 image layout and required symbols are present"


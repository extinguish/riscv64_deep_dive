#!/usr/bin/env bash
set -euo pipefail

elf=${1:?usage: check-elf.sh ELF}
readelf_bin=${READELF:-}
nm_bin=${NM:-}

if [[ -z "$readelf_bin" ]]; then
    readelf_bin=$(command -v riscv64-unknown-elf-readelf 2>/dev/null ||
                  command -v readelf)
fi
if [[ -z "$nm_bin" ]]; then
    nm_bin=$(command -v riscv64-unknown-elf-nm 2>/dev/null || command -v nm)
fi

header=$($readelf_bin -h "$elf")
attributes=$($readelf_bin -A "$elf")
sections=$($readelf_bin -SW "$elf")
segments=$($readelf_bin -lW "$elf")
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
require_text "$header" 'Type:[[:space:]]+EXEC' 'image is not a static executable'
require_text "$header" 'Entry point address:[[:space:]]+0x80000000' \
    'unexpected entry address'
require_text "$attributes" 'Tag_RISCV_arch:' 'missing RISC-V architecture attributes'
require_text "$sections" '[[:space:]]\.lab_blob[[:space:]]' 'missing .lab_blob section'
require_text "$sections" '[[:space:]]\.eh_frame[[:space:]]' 'missing .eh_frame section'

if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then
    echo "ELF CHECK FAIL: loadable ELF segment is writable and executable" >&2
    exit 1
fi

if [[ -n "$undefined" ]]; then
    echo "ELF CHECK FAIL: undefined symbols remain:" >&2
    echo "$undefined" >&2
    exit 1
fi

for symbol in _start asm_memzero asm_memmove asm_add128 asm_call_checksum \
              asm_global_address asm_global_load asm_pic_blob \
              __lab_blob_start __lab_blob_end; do
    if ! $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$"; then
        echo "ELF CHECK FAIL: missing symbol $symbol" >&2
        exit 1
    fi
done

echo "ELF CHECK PASS: ELF64 RISC-V static image and required symbols are present"

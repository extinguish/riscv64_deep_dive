#!/usr/bin/env bash
set -euo pipefail
elf=${1:?ELF required}
readelf_bin=${READELF:-readelf}
nm_bin=${NM:-nm}
header=$($readelf_bin -h "$elf")
segments=$($readelf_bin -lW "$elf")
[[ $header == *ELF64* && $header == *RISC-V* ]] || { echo 'ELF CHECK FAIL: expected RV64' >&2; exit 1; }
if grep -Eq 'LOAD.*W.*E' <<<"$segments"; then echo 'ELF CHECK FAIL: W+X segment' >&2; exit 1; fi
for symbol in _start lab_m_trap lab_s_payload lab_sbi_dispatch lab_domain_range lab_pmp_napot lab_firmware_secret; do
  $nm_bin "$elf" | grep -Eq "[[:space:]]${symbol}$" || { echo "ELF CHECK FAIL: missing $symbol" >&2; exit 1; }
done
symbol_address() { $nm_bin "$elf" | awk -v name="$1" '$3 == name {print "0x" $1; exit}'; }
fw_secret=$(symbol_address lab_firmware_secret)
m_stack=$(symbol_address __m_stack_top)
s_payload=$(symbol_address lab_s_payload)
s_stack=$(symbol_address __s_stack_top)
if (( fw_secret >= 0x80200000 || m_stack > 0x80200000 ||
      s_payload < 0x80200000 || s_payload >= 0x80400000 ||
      s_stack > 0x80400000 )); then
  echo 'ELF CHECK FAIL: firmware/S-domain placement' >&2; exit 1
fi
echo 'ELF CHECK PASS: RV64 firmware/S payload symbols and W^X segments'

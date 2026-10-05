#!/usr/bin/env bash
set -euo pipefail
source_dir="$LAB_DIR/build/keystone"
[[ "$(git -C "$source_dir" rev-parse HEAD)" == '73aeab79b97f4cea3e418897841e5534fc978f4b' ]]
printf 'Keystone revision: %s\n' "$(git -C "$source_dir" rev-parse HEAD)"
printf '\nPMP context switch sites:\n'
rg -n 'context_switch_to_enclave|context_switch_to_host|osm_pmp_set|pmp_set_keystone|switch_vector_' "$source_dir/sm/src/enclave.c" | head -45
rg -n 'PMP_SET|sfence.vma' "$source_dir/sm/src/pmp.h" | head -15
printf '\nSM SBI entry and attestation sites:\n'
rg -n 'SBI_SM_(CREATE|RUN|RESUME|STOP|EXIT|DESTROY|ATTEST)_ENCLAVE|attest_enclave' \
  "$source_dir/sm/src/sm-sbi-opensbi.c" "$source_dir/sm/src/enclave.c" | head -50
printf '\nQEMU/GDB command:\n'
printf '  cd %s && KEYSTONE_DEBUG=y make run\n' "$source_dir"
printf '  cd %s && make debug-connect\n' "$source_dir"
printf '  in GDB: pmp-dump; break pmp_set_keystone; continue\n'

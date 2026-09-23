#!/usr/bin/env bash
set -uo pipefail

timeout_value=${1:?missing timeout value}
shift

set +e
timeout --foreground "$timeout_value" "$@"
status=$?
set -e

if [[ $status -eq 124 ]]; then
    cat >&2 <<'EOF'
QEMU TIMEOUT: execution did not reach the bounded completion path.

Run smoke and unit first. Inspect the satp root, identity mappings, PTE
validity, superpage alignment, stvec/scause, and whether the PTE store is
ordered before SFENCE.VMA. A bad live mapping can remove the code, stack, or
MMIO path, so compare the last printed test with build/lab06.disasm.txt.
EOF
fi

exit "$status"

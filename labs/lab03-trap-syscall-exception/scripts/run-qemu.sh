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
QEMU TIMEOUT: the guest did not reach the finisher.

Run ./build.sh smoke and ./build.sh unit first. If both behave as expected,
inspect lab_s_trap_entry for a bad sscratch swap, frame offset, stack alignment,
restore order, sepc update, or sret state. A corrupted sp/ra can also prevent
the fatal path from printing useful diagnostics.
EOF
fi

exit "$status"

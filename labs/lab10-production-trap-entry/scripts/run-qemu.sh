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

Run smoke and unit first. Inspect the first sscratch exchange, original-SP
calculation, frame size/alignment, saved t0/frame base, sepc advancement, and
the final restore order. The supplied TODO vector exits immediately, so a
timeout usually means a partially implemented entry corrupted control state.
EOF
fi

exit "$status"

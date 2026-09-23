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

Run smoke and unit first. Inspect pmpcfg0 byte preservation, pmpaddr0 NAPOT
encoding, entry priority, MPRV/MPP restoration, and mtvec/mcause. The learner
window must never modify or lock the infrastructure-owned entry 1.
EOF
fi

exit "$status"

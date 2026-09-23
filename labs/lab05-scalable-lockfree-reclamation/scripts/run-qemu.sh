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
QEMU TIMEOUT: a hart did not reach the bounded completion path.

Run smoke and unit first. Inspect MCS successor linking, LR/SC retry loops,
seqlock odd-version handling, hazard head revalidation, and accidental changes
to infrastructure barrier state. SPSC and reader progress polling in the test
harness are bounded and should normally report failure instead of hanging.
EOF
fi

exit "$status"

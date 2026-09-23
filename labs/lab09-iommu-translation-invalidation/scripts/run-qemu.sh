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

Run smoke and unit first. Inspect page-table index bounds, context-count loop
termination, command tickets, bounded completion waits, state transitions, and
preservation of callee-saved registers. Never replace the bounded wait with an
unbounded spin loop.
EOF
fi

exit "$status"

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

Run smoke and unit first. In the full test, inspect LR/SC retry loops, a lock
that never releases, ticket owner/next width, and corruption of infrastructure
barrier state. Mailbox polling itself is bounded and should report failure.
EOF
fi

exit "$status"

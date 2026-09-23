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

Run smoke and unit first. Inspect range overflow, loop termination, preserved
callee-saved registers, DMA ownership checks, and the generated-code return
instruction. The test only calls generated code after lab_publish_code succeeds.
EOF
fi

exit "$status"

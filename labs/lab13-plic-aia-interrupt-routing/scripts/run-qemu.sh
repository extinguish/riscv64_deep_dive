#!/usr/bin/env bash
set -uo pipefail
timeout_value=${1:?missing timeout value}; shift
set +e
timeout --foreground "$timeout_value" "$@"
status=$?
set -e
if [[ $status -eq 124 ]]; then
    cat >&2 <<'EOF'
QEMU TIMEOUT: the external-interrupt state machine did not finish.
Run smoke and unit first. Inspect UART IER, PLIC source priority/enable,
context threshold, MEIE/MIE, claim ID, device deassertion before complete,
and whether the completion write used the same context that claimed it.
EOF
fi
exit "$status"

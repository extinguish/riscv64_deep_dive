#!/usr/bin/env bash
set -uo pipefail
timeout_value=${1:?missing timeout value}; shift
set +e
timeout --foreground "$timeout_value" "$@"
status=$?
set -e
if [[ $status -eq 124 ]]; then
    cat >&2 <<'EOF'
QEMU TIMEOUT: an SMP notification or timer did not reach bounded completion.
Run smoke and unit first. Then inspect mailbox release/acquire ordering, the
clear-before-exchange MSIP path, check-WFI-recheck loops, mtimecmp programming,
and whether every secondary reached the RUN lifecycle state.
EOF
fi
exit "$status"

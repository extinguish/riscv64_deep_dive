#!/usr/bin/env bash
set -uo pipefail
timeout_value=${1:?missing timeout value}; shift
set +e
timeout --foreground "$timeout_value" "$@"
status=$?
set -e
if [[ $status -eq 124 ]]; then
    cat >&2 <<'EOF'
QEMU TIMEOUT: context switching did not reach the bounded completion path.
Run unit first. Inspect synthetic ra/sp, 16-byte stack alignment, save/restore
offsets, incoming ra, tp ownership, and whether task exit switched back to the
scheduler instead of returning onto an exhausted task stack.
EOF
fi
exit "$status"

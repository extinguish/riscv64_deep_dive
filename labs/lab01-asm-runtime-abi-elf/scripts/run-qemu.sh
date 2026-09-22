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
QEMU TIMEOUT: the guest did not report completion through the virt finisher.

If no "LAB01" banner was printed, the guest stalled before or during main().
Run these commands next:
  ./build.sh clean
  ./build.sh smoke

If smoke passes but test times out, inspect your exercise code for an infinite
loop or damage to sp/ra. The fatal-trap path prints mcause/mepc/mtval when its
stack and UART remain usable.
EOF
fi

exit "$status"

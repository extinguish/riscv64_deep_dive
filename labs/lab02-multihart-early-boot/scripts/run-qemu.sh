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
QEMU TIMEOUT: one or more harts did not reach a completion path.

Run ./build.sh smoke first. If smoke passes, inspect lab_boot_entry for:
  - an invalid permanent-stack calculation;
  - a lost FDT/hartid argument;
  - no coldboot winner or more than one winner;
  - a secondary waiting without an acquire/recheck loop;
  - damage to sp, ra, gp, or tp before entering C.
EOF
fi

exit "$status"


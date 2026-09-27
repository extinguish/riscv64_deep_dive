#!/usr/bin/env bash
set -uo pipefail
limit=${1:?timeout}; shift
set +e
timeout --foreground "$limit" "$@"
status=$?
set -e
if [[ $status -eq 124 ]]; then
  echo 'QEMU TIMEOUT: inspect PMP, M/S stack swap, SBI return and mepc advance.' >&2
fi
exit "$status"

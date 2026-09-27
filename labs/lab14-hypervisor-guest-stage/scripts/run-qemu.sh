#!/usr/bin/env bash
set -uo pipefail
limit=${1:?timeout}; shift
set +e
timeout --foreground "$limit" "$@"
status=$?
set -e
if [[ $status -eq 124 ]]; then
  echo 'QEMU TIMEOUT: inspect HS trap origin, G-stage PTE, HFENCE.GVMA and guest retry.' >&2
fi
exit "$status"

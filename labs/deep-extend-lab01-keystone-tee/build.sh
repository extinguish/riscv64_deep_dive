#!/usr/bin/env bash
set -euo pipefail
lab_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
action=${1:-build}
case "$action" in
  toolcheck|bootstrap|build|smoke|unit|test|inspect|clean)
    exec make -C "$lab_dir" "$action" ;;
  *) echo "usage: $0 {toolcheck|bootstrap|build|smoke|unit|test|inspect|clean}" >&2; exit 2 ;;
esac

#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
action=${1:-build}

case "$action" in
    toolcheck|build|smoke|unit|test|inspect|clean)
        exec make -C "$script_dir" "$action"
        ;;
    *)
        echo "usage: $0 {toolcheck|build|smoke|unit|test|inspect|clean}" >&2
        exit 2
        ;;
esac

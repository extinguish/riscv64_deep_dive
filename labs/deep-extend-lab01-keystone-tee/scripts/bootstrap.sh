#!/usr/bin/env bash
set -euo pipefail
source_dir="$LAB_DIR/build/keystone"
if [[ ! -d "$source_dir/.git" ]]; then
  mkdir -p "$LAB_DIR/build"
  git clone https://github.com/keystone-enclave/keystone.git "$source_dir"
fi
actual=$(git -C "$source_dir" rev-parse HEAD)
if [[ "$actual" != "$KEYSTONE_REV" ]]; then
  if [[ -n $(git -C "$source_dir" status --porcelain --untracked-files=no) ]]; then
    echo "Keystone checkout has local changes at $actual; refusing to switch revisions" >&2
    exit 1
  fi
  git -C "$source_dir" checkout --detach "$KEYSTONE_REV"
fi
git -C "$source_dir" submodule update --init buildroot runtime/test/cmocka
printf 'Keystone pinned at %s\n' "$(git -C "$source_dir" rev-parse HEAD)"
git -C "$source_dir" submodule status buildroot runtime/test/cmocka

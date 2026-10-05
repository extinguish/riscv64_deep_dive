#!/usr/bin/env bash
set -euo pipefail
source_dir="$LAB_DIR/build/keystone"
dest_dir="$source_dir/examples/attestation/host"
[[ -d "$dest_dir" ]] || { echo "Keystone attestation source missing" >&2; exit 1; }
changed=0
for filename in verifier.cpp report_policy.hpp; do
  if [[ "$filename" == verifier.cpp ]]; then
    source_file="$LAB_DIR/infra/verifier.cpp"
  else
    source_file="$LAB_DIR/exercises/report_policy.hpp"
  fi
  if ! cmp -s "$source_file" "$dest_dir/$filename"; then
    cp "$source_file" "$dest_dir/$filename"
    changed=1
  fi
done
if [[ "$changed" -eq 1 && -f "$source_dir/build-generic64/buildroot.build/.config" ]]; then
  echo 'Refreshing Keystone examples after lab source changes'
  BUILDROOT_TARGET=keystone-examples-dirclean make -C "$source_dir" KEYSTONE_PLATFORM=generic KEYSTONE_BITS=64
fi

#!/usr/bin/env bash
set -euo pipefail
mode=${1:?smoke or test required}
source_dir="$LAB_DIR/build/keystone"
key="$source_dir/build-generic64/buildroot.build/target/root/.ssh/id-rsa"
qemu="$source_dir/build-generic64/buildroot.build/host/bin/qemu-system-riscv64"
[[ -x "$qemu" && -f "$key" ]] || { echo 'Keystone QEMU image is not built' >&2; exit 1; }
export LD_LIBRARY_PATH="$source_dir/build-generic64/buildroot.build/host/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
chmod 600 "$key"
port=${KEYSTONE_PORT:-19821}
log_dir="$LAB_DIR/build/logs"
mkdir -p "$log_dir"
qemu_log="$log_dir/qemu-$mode.log"
ssh_args=(-i "$key" -p "$port" -o BatchMode=yes -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o ConnectTimeout=2)
cleanup() {
  if [[ -n ${qemu_pid:-} ]]; then
    kill -- "-$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
setsid make -C "$source_dir" KEYSTONE_PLATFORM=generic KEYSTONE_BITS=64 KEYSTONE_PORT="$port" run >"$qemu_log" 2>&1 &
qemu_pid=$!
ready=0
for ((attempt=0; attempt<180; attempt++)); do
  if timeout 4 ssh "${ssh_args[@]}" root@127.0.0.1 true >/dev/null 2>&1; then
    ready=1
    break
  fi
  if ! kill -0 "$qemu_pid" 2>/dev/null; then
    echo "QEMU exited; see $qemu_log" >&2
    exit 1
  fi
  sleep 1
done
[[ "$ready" -eq 1 ]] || { echo "guest SSH timeout; see $qemu_log" >&2; exit 1; }
timeout 30 ssh "${ssh_args[@]}" root@127.0.0.1 'modprobe keystone-driver'
if [[ "$mode" == smoke ]]; then
  timeout 120 ssh "${ssh_args[@]}" root@127.0.0.1 \
    '/usr/share/keystone/examples/hello.ke' | tee "$log_dir/smoke.log"
  rg -q '^hello, world!$' "$log_dir/smoke.log"
  echo 'DEEP_EXTEND_LAB01_SMOKE=PASS'
elif [[ "$mode" == test ]]; then
  normal_log="$log_dir/attestation-normal.log"
  timeout 120 ssh "${ssh_args[@]}" root@127.0.0.1 \
    '/usr/share/keystone/examples/attestor.ke' >"$normal_log" 2>&1 || {
      cat "$normal_log"; exit 1;
    }
  cat "$normal_log"
  rg -q '^LAB01_ATTEST=PASS$' "$normal_log"
  for fault in hash nonce signature; do
    fault_log="$log_dir/attestation-$fault.log"
    if timeout 120 ssh "${ssh_args[@]}" root@127.0.0.1 \
      "LAB_FAULT=$fault /usr/share/keystone/examples/attestor.ke" >"$fault_log" 2>&1; then
      cat "$fault_log"
      echo "fault $fault was accepted" >&2
      exit 1
    fi
    if rg -q '^LAB01_ATTEST=PASS$' "$fault_log"; then
      echo "fault $fault produced success marker" >&2
      exit 1
    fi
    if ! rg -q '^LAB01_ATTEST=REJECT$' "$fault_log"; then
      cat "$fault_log"
      echo "fault $fault failed without a verifier rejection" >&2
      exit 1
    fi
    printf 'negative case %s: rejected\n' "$fault"
  done
  echo 'DEEP_EXTEND_LAB01_TEST=PASS'
else
  echo "unknown mode: $mode" >&2
  exit 2
fi

#!/usr/bin/env bash
# run_e2e.sh — build the Linux sidecar binaries and run the two-process
# Polyglot e2e (sls-kerneld + wasmi Wasm sidecar + real SBCL Lisp sidecar
# over the 302-304 wire ABI with a shared arena).
#
# On Windows dev boxes everything runs inside WSL (mirrored networking or
# localhost forwarding required). On Linux (CI self-hosted runner) it runs
# natively — install sbcl:  sudo apt-get install -y sbcl
#
# Usage:  bash user/polyglot/run_e2e.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TARGET_DIR="${POLYGLOT_TARGET_DIR:-/tmp/polyglot-target}"

if uname -s | grep -qi linux; then
    LINUX_ROOT="$ROOT"
    LINUX() { bash -lc "$*"; }
else
    # Git Bash pwd is /c/Users/... — translate to /mnt/c/Users/...
    LINUX_ROOT="$(echo "$ROOT" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')"
    LINUX() { wsl -e bash -lc "$*"; }
fi

echo "==> building sidecar binaries for Linux (target dir: $TARGET_DIR)"
LINUX "cd '$LINUX_ROOT/user' && \
  export CARGO_TARGET_DIR='$TARGET_DIR' && \
  export PATH=\"\$HOME/.cargo/bin:\$PATH\" && \
  cargo build -p polyglot --release --features linux 2>&1 | tail -8 && \
  test -x '$TARGET_DIR/release/sls-kerneld' && \
  test -x '$TARGET_DIR/release/wasm-sidecar' && \
  echo BINARIES_OK"

echo "==> running the two-process e2e (native cargo test on this side)"
export POLYGLOT_TARGET_DIR="$TARGET_DIR"
cargo test -p polyglot --test e2e_two_process \
    --manifest-path "$ROOT/user/Cargo.toml" -- --nocapture

echo "==> e2e complete"

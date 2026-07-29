#!/usr/bin/env bash
# run_two_nodes_harness.sh — exercises run-two-nodes.sh against a STUB QEMU.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# run-two-nodes.sh shipped for a long time carrying a note that it had never
# been run. When it finally was, on a headless server, it failed three ways
# at once -- and the worst of the three was that it printed "Both instances
# launched" with two PIDs for a node that had already exited. A launcher
# that reports success for a dead process is worse than one that crashes,
# because it sends you looking in the wrong place.
#
# `bash -n` would not have caught any of the three. Neither would review:
# the third bug (a serial-only kernel given an output-only serial sink) is
# only visible if you know read_line() polls the UART. So this harness puts
# a fake qemu-system-x86_64 on PATH and drives the real script through the
# paths that actually broke.
#
# The stub deliberately reproduces the reported failure: it refuses to run
# with `-display gtk`, exactly as a machine with no X server does.
#
# Run:  bash tests/run_two_nodes_harness.sh
set -uo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/run-two-nodes.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
cp "$SCRIPT" "$T/run-two-nodes.sh"
chmod +x "$T/run-two-nodes.sh"

passed=0; failed=0
check() {
    if [ "$1" = "yes" ]; then echo "ok:   $2"; passed=$((passed+1))
    else echo "FAIL: $2"; failed=$((failed+1)); fi
}
has() { case "$2" in *"$1"*) echo yes;; *) echo no;; esac; }
hasnt() { case "$2" in *"$1"*) echo no;; *) echo yes;; esac; }

cat > "$T/bin/make" <<'EOF'
#!/bin/bash
echo "  (stub make $*)"
EOF
cat > "$T/bin/qemu-img" <<'EOF'
#!/bin/bash
touch "$3" 2>/dev/null || true
EOF

# The stub mimics a headless host: gtk is ADVERTISED by -display help (the
# binary has it compiled in) but FAILS at runtime (there is no display to
# open). That gap is the whole reason probing `-display help` alone is not
# sufficient and the environment has to be checked too.
cat > "$T/bin/qemu-system-x86_64" <<'EOF'
#!/bin/bash
if [ "${1:-}" = "-display" ] && [ "${2:-}" = "help" ]; then
  printf 'Available display backend types:\nnone\ngtk\nvnc\n'; exit 0
fi
for a in "$@"; do
  [ "$a" = "gtk" ] && { echo "gtk initialization failed" >&2; exit 1; }
done
echo "$@" > "${STUB_ARGV_OUT:-/dev/null}"
sleep 300
EOF
chmod +x "$T/bin"/*
export PATH="$T/bin:$PATH"

echo "=== run-two-nodes.sh against a stub QEMU ==="
echo

# ═══ 1: a headless host must not pick a display it cannot open ═══════════
echo "-- 1: headless (no DISPLAY, no WAYLAND_DISPLAY) --"
export STUB_ARGV_OUT="$T/argv_a.txt"
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY timeout 8 ./run-two-nodes.sh 2>&1)" || true
check "$(has 'Display backend: none' "$OUT")" \
      "*** picks -display none rather than gtk ***"
check "$(hasnt 'gtk initialization failed' "$OUT")" \
      "*** ...so the reported failure does not happen ***"
check "$(has 'Both nodes are up and confirmed running' "$OUT")" \
      "both nodes come up"
check "$(has 'telnet 127.0.0.1 12341' "$OUT")" \
      "prints a console command that can actually be typed into"
check "$(has 'serial-only' "$OUT")" \
      "explains there are no windows to miss"

ARGV="$(cat "$T/argv_a.txt" 2>/dev/null || true)"
check "$(has 'telnet=on' "$ARGV")" \
      "*** the console is an interactive socket, not a write-only file ***"
check "$(has 'logfile=' "$ARGV")" \
      "*** ...and still writes the debug log -- both, not either ***"
check "$(hasnt '-serial file:' "$ARGV")" \
      "the output-only serial sink is gone"
check "$(has 'host=127.0.0.1' "$ARGV")" \
      "*** the console binds loopback only, never 0.0.0.0 ***"
check "$(has 'mcast=' "$ARGV")" \
      "*** the netdev is a SHARED multicast segment, not point-to-point ***"
check "$(hasnt 'listen=:' "$ARGV")" \
      "...the listen/connect pair that capped the cluster at two nodes is gone"
check "$(hasnt 'connect=127.0.0.1:1234' "$ARGV")" \
      "...on both sides"
check "$(has 'smp 1' "$ARGV")" \
      "*** one vCPU per node -- a second buys a spinning AP loop, not throughput ***"

# ═══ 2: a dead node must be reported as dead ═════════════════════════════
# The original bug. Forcing gtk on the stub makes QEMU exit immediately,
# which is exactly what happened on the real server.
echo
echo "-- 2: a node that dies on launch --"
OUT="$(cd "$T" && AEROSLS_DISPLAY=gtk timeout 8 ./run-two-nodes.sh 2>&1)"; RC=$?
check "$(has 'failed to start' "$OUT")" \
      "*** a node that exited is reported as failed ***"
check "$(hasnt 'Both nodes are up' "$OUT")" \
      "*** ...and success is NOT claimed -- the original bug ***"
check "$(has 'gtk initialization failed' "$OUT")" \
      "*** QEMU's own stderr is shown, so the cause is visible ***"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" \
      "exits non-zero"

# ═══ 3: the temp-file lifetime bug this harness found ════════════════════
# `PID=$(launch_node ...)` ran the launch in a command substitution, and
# bash fires EXIT traps when such a subshell finishes -- deleting the very
# stderr file the error report then tried to read.
echo
echo "-- 3: the error report can still read its own evidence --"
check "$(hasnt 'No such file or directory' "$OUT")" \
      "*** the stderr capture outlives the launch helper ***"
check "$(hasnt "can't read" "$OUT")" \
      "...no sed complaint in place of the real error"

# ═══ 4: an explicit override is honoured ═════════════════════════════════
echo
echo "-- 4: AEROSLS_DISPLAY override --"
OUT="$(cd "$T" && DISPLAY=:0 AEROSLS_DISPLAY=none timeout 8 ./run-two-nodes.sh 2>&1)" || true
check "$(has 'Display backend: none' "$OUT")" \
      "an explicit override beats autodetection even when DISPLAY is set"

# ═══ 5: a port already in use is caught before building an ISO ═══════════
echo
echo "-- 5: a busy console port --"
if command -v python3 >/dev/null 2>&1; then
    python3 -c "
import socket,time,sys
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',12341)); s.listen(1)
sys.stderr.write('held\n'); sys.stderr.flush()
time.sleep(6)" 2>/dev/null &
    HOLDER=$!
    sleep 1
    OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY timeout 8 ./run-two-nodes.sh 2>&1)" || true
    kill "$HOLDER" 2>/dev/null || true
    check "$(has 'already in use' "$OUT")" \
          "a held console port is refused with a usable message"
    check "$(hasnt 'stub make' "$OUT")" \
          "*** ...and it is caught BEFORE spending minutes building an ISO ***"
fi

# ═══ 6: the multicast port is SHARED, and must not be treated as a clash ══
# Every node binds the group's port -- that shared bind is the segment. The
# old point-to-point listener genuinely was exclusive, so this check had to
# change with the netdev.
echo
echo "-- 6: the segment port is shared by design --"
if command -v python3 >/dev/null 2>&1; then
    python3 -c "
import socket,time,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('',12340))
time.sleep(6)" 2>/dev/null &
    HOLDER=$!
    sleep 1
    OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY timeout 8 ./run-two-nodes.sh 2>&1)" || true
    kill "$HOLDER" 2>/dev/null || true
    check "$(hasnt 'already in use' "$OUT")" \
          "*** something already on the group port does NOT block the launch ***"
    check "$(has 'Both nodes are up' "$OUT")" \
          "...the cluster still forms"
else
    echo "skip: python3 unavailable"
fi

echo
echo "=========================================="
echo "passed=$passed failed=$failed"
[ "$failed" -eq 0 ]

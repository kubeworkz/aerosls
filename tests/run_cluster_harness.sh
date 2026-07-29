#!/usr/bin/env bash
# run_cluster_harness.sh — exercises run-cluster.sh against stub tooling.
#
# ─── Why a harness rather than a real run ─────────────────────────────────
# run-two-nodes.sh shipped for months carrying "has never actually been
# run", and when it finally was it failed three ways at once -- the worst
# being that it printed "Both instances launched" with two PIDs for a node
# that had already exited. This launcher is strictly more machinery: N
# nodes, N ISOs, N ports, teardown. Shipping it unexercised would be
# repeating the mistake with more surface.
#
# So: stub `qemu-system-x86_64`, `grub-mkrescue`, `make` and `qemu-img` on
# PATH, and drive the real script. The two properties worth the most here
# are the ones a casual read would not catch:
#
#   - each node's ISO carries ITS OWN node= id     (scenario 3)
#   - a partial failure tears down what came up    (scenario 5)
#
# The second is the one that bites in practice. Node 4 of 4 failing must
# not leave three orphaned QEMUs holding console ports.
#
# Run:  bash tests/run_cluster_harness.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/net"
cp "$ROOT/run-cluster.sh" "$T/"
chmod +x "$T/run-cluster.sh"
# The script reads the node cap out of this header rather than hardcoding
# it; give it a real one to read.
cp "$ROOT/net/consensus.h" "$T/net/"

passed=0; failed=0
check() {
    if [ "$1" = "yes" ]; then echo "ok:   $2"; passed=$((passed+1))
    else echo "FAIL: $2"; failed=$((failed+1)); fi
}
has()   { case "$2" in *"$1"*) echo yes;; *) echo no;; esac; }
hasnt() { case "$2" in *"$1"*) echo no;; *) echo yes;; esac; }
# All of the given needles present. `$(has a "$x")$(has b "$x")` looked like
# an AND and is actually string concatenation -- "yesyes" is not "yes", so
# such a check can only ever fail. Found by this harness's own first run.
hasall() {
    local hay="$1"; shift
    for n in "$@"; do case "$hay" in *"$n"*) ;; *) echo no; return;; esac; done
    echo yes
}

cat > "$T/bin/make" <<'EOF'
#!/bin/bash
touch my_sls_kernel.bin
EOF
cat > "$T/bin/qemu-img" <<'EOF'
#!/bin/bash
touch "$3" 2>/dev/null || true
EOF
# Records the grub.cfg it was handed, so the per-node identity can be
# checked -- that generated file is the entire Phase 1 linkage.
cat > "$T/bin/grub-mkrescue" <<'EOF'
#!/bin/bash
out=""; stage=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    --modules=*) shift ;;
    *) stage="$1"; shift ;;
  esac
done
[ -n "$stage" ] && cat "$stage/boot/grub/grub.cfg" >> "${STUB_GRUB_LOG:-/dev/null}"
touch "$out"
EOF
# Appends its argv, one line per invocation, so per-node differences are
# visible. Fails on -display gtk exactly as a headless host does.
cat > "$T/bin/qemu-system-x86_64" <<'EOF'
#!/bin/bash
if [ "${1:-}" = "-display" ] && [ "${2:-}" = "help" ]; then
  printf 'Available display backend types:\nnone\ngtk\nvnc\n'; exit 0
fi
for a in "$@"; do
  [ "$a" = "gtk" ] && { echo "gtk initialization failed" >&2; exit 1; }
done
echo "$@" >> "${STUB_ARGV_LOG:-/dev/null}"
# STUB_FAIL_NODE: this node id refuses to start, to exercise partial failure.
if [ -n "${STUB_FAIL_NODE:-}" ]; then
  for a in "$@"; do
    case "$a" in
      *"node${STUB_FAIL_NODE}.iso") echo "simulated failure on node ${STUB_FAIL_NODE}" >&2; exit 1 ;;
    esac
  done
fi
# exec, not a plain call: without it the stub shell forks `sleep` and dies
# on kill while the orphaned sleep lives on -- still holding the stdout pipe
# a command substitution is waiting to close, which hangs the caller for the
# full 300s. Real QEMU is one process and is killable; this makes the stub
# behave the same way. (Found by this harness hanging on scenario 5.)
exec sleep 300
EOF
chmod +x "$T/bin"/*
export PATH="$T/bin:$PATH"
# The launcher settles once for all nodes rather than once per node, so a
# short settle here is faithful, not a shortcut -- the stub QEMU either
# execs and sleeps, or exits immediately.
export AEROSLS_SETTLE=1

# Model a host with room to spare. Scenarios 1-7 test the launcher's
# MECHANICS -- ports, ISOs, teardown -- and would otherwise be gated by
# whatever machine happens to run this suite. (They were, the first time
# capacity checking landed: this sandbox has ~2.5 GiB available, so every
# multi-node scenario was correctly refused.) Scenario 8 overrides these
# per-case to test the sizing itself.
export AEROSLS_HOST_CORES=8
export AEROSLS_HOST_MEM_MB=32000
export AEROSLS_HOST_DISK_MB=200000

run() { (cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY "$@" 2>&1); }

# Starts the launcher in the BACKGROUND and waits for it to report N nodes.
#
# Running it under `timeout` in the foreground does not work: the launcher
# ends in `wait`, so timeout's TERM lands on it, fires the teardown trap and
# removes the pid file -- correct behaviour, and it destroys exactly the
# state the next checks want to look at. A real operator starts it and
# leaves it running, which is what this does.
LAUNCH_PID=""
launch_bg() {
    local want="$1"; shift
    : > "$T/launch.out"
    ( cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY ./run-cluster.sh "$@" \
        >"$T/launch.out" 2>&1 ) &
    LAUNCH_PID=$!
    for _ in $(seq 1 60); do
        if [ -f "$T/cluster/cluster.pids" ] && \
           [ "$(wc -l < "$T/cluster/cluster.pids" 2>/dev/null || echo 0)" -ge "$want" ]; then
            sleep 0.3; return 0
        fi
        kill -0 "$LAUNCH_PID" 2>/dev/null || return 1   # it exited early
        sleep 0.5
    done
    return 1
}
launch_wait_exit() {
    for _ in $(seq 1 60); do
        kill -0 "$LAUNCH_PID" 2>/dev/null || return 0
        sleep 0.5
    done
    return 1
}

echo "=== run-cluster.sh against stub tooling ==="
echo

# ═══ 1: --dry-run builds and launches nothing ════════════════════════════
echo "-- 1: --dry-run --"
export STUB_ARGV_LOG="$T/argv1.txt"; : > "$STUB_ARGV_LOG"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 3 --dry-run)"
check "$(has 'nodes            3' "$OUT")" "the plan states the node count"
check "$(has 'shared, all nodes' "$OUT")" "...and that the segment is shared"
check "$(hasall "$OUT" 'node=1' 'node=2' 'node=3')" \
      "each node's identity is shown"
check "$(has '12341' "$OUT")" "console ports are shown"
check "$([ ! -s "$STUB_ARGV_LOG" ] && echo yes || echo no)" \
      "*** --dry-run launches nothing ***"
check "$([ ! -d "$T/cluster" ] && echo yes || echo no)" \
      "*** ...and builds nothing -- no ISOs for a run that will not happen ***"

# ═══ 2: the node cap is read from the source, not hardcoded ══════════════
echo
echo "-- 2: the CLUSTER_NODE_MAX cap --"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 9 --dry-run)"; RC=$?
check "$(has 'out of range' "$OUT")" "*** 9 nodes is refused (cap is 8) ***"
check "$(has 'CLUSTER_NODE_MAX' "$OUT")" "...naming where the cap comes from"
check "$(has 'never join' "$OUT")" \
      "*** ...and why it matters: cluster_init() would refuse the id and the "\
"node would boot STANDALONE, looking fine ***"
check "$(has 'protocol limit, not this host' "$OUT")" \
      "*** ...distinguished from a host limit -- no machine holds 9 ***"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" "exits non-zero"

OUT="$(run timeout 20 ./run-cluster.sh --nodes 0 --dry-run)"
check "$(has 'out of range' "$OUT")" "zero nodes is refused"
OUT="$(run timeout 20 ./run-cluster.sh --nodes two --dry-run)"
check "$(has 'whole number' "$OUT")" "a non-numeric count is refused"

# Raising the cap in the header raises the launcher's, with no edit here.
sed -i 's/#define CLUSTER_NODE_MAX 8/#define CLUSTER_NODE_MAX 16/' "$T/net/consensus.h"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 9 --dry-run)"
check "$(hasnt 'out of range' "$OUT")" \
      "*** raising the cap in consensus.h raises the launcher's, unedited ***"
sed -i 's/#define CLUSTER_NODE_MAX 16/#define CLUSTER_NODE_MAX 8/' "$T/net/consensus.h"

# ═══ 3: every node gets its own identity ═════════════════════════════════
echo
echo "-- 3: per-node boot media --"
export STUB_ARGV_LOG="$T/argv3.txt"; : > "$STUB_ARGV_LOG"
export STUB_GRUB_LOG="$T/grub3.txt"; : > "$STUB_GRUB_LOG"
launch_bg 3 --nodes 3
OUT="$(cat "$T/launch.out")"
check "$(has '3 nodes up and confirmed running' "$OUT")" "3 nodes come up"

GRUB="$(cat "$T/grub3.txt" 2>/dev/null)"
check "$(hasall "$GRUB" 'node=1' 'node=2' 'node=3')" \
      "*** the generated grub.cfg carries node=1, node=2 and node=3 ***"
check "$([ "$(grep -c 'multiboot2 /boot/my_sls_kernel.bin node=' "$T/grub3.txt")" = 3 ] && echo yes || echo no)" \
      "*** exactly three distinct boot lines -- one ISO cannot say two things ***"
check "$(hasnt 'node=4' "$GRUB")" "...and no fourth"

ARGV="$(cat "$T/argv3.txt")"
check "$([ "$(grep -c 'mcast=239' "$T/argv3.txt")" = 3 ] && echo yes || echo no)" \
      "*** all three join the SAME segment ***"
check "$([ "$(grep -oE 'mac=52:54:00:AE:51:[0-9a-f]{2}' "$T/argv3.txt" | sort -u | wc -l)" = 3 ] && echo yes || echo no)" \
      "*** each node gets a DISTINCT MAC -- the self-echo guard keys on it ***"
check "$([ "$(grep -oE 'port=1234[0-9]' "$T/argv3.txt" | sort -u | wc -l)" = 3 ] && echo yes || echo no)" \
      "...and a distinct console port"
check "$(has 'smp 1' "$ARGV")" "one vCPU per node by default"
check "$(has 'telnet=on' "$ARGV")" "the console is interactive, not write-only"
check "$(has 'host=127.0.0.1' "$ARGV")" "...bound to loopback only"

check "$([ -f "$T/cluster/cluster.pids" ] && echo yes || echo no)" "a pid file is written"
check "$([ "$(wc -l < "$T/cluster/cluster.pids")" = 3 ] && echo yes || echo no)" \
      "...with one line per node"

# ═══ 4: --stop tears it down ═════════════════════════════════════════════
echo
echo "-- 4: --stop --"
OUT="$(run timeout 20 ./run-cluster.sh --stop)"
check "$(has 'stopped 3' "$OUT")" "*** --stop stops all three ***"
check "$([ ! -f "$T/cluster/cluster.pids" ] && echo yes || echo no)" \
      "...and clears the pid file"
check "$(launch_wait_exit && echo yes || echo no)" \
      "*** ...and the launcher itself returns rather than hanging in wait ***"
OUT="$(run timeout 20 ./run-cluster.sh --stop)"
check "$(has 'no cluster to stop' "$OUT")" "a second --stop is a clean no-op"

# ═══ 5: a partial failure must not orphan the survivors ══════════════════
echo
echo "-- 5: node 3 of 4 fails --"
export STUB_ARGV_LOG="$T/argv5.txt"; : > "$STUB_ARGV_LOG"
export STUB_FAIL_NODE=3
OUT="$(run timeout 25 ./run-cluster.sh --nodes 4)"; RC=$?
unset STUB_FAIL_NODE
check "$(has 'node 3 failed to start' "$OUT")" "*** the failing node is named ***"
check "$(has 'simulated failure on node 3' "$OUT")" \
      "*** QEMU's own stderr is shown, so the cause is visible ***"
check "$(has 'tearing down the 3 node' "$OUT")" \
      "*** the 3 nodes that DID come up are torn down, not orphaned ***"
check "$(has '1 of 4 node' "$OUT")" \
      "...and the count of failures is stated up front"
check "$(hasnt 'nodes up and confirmed running' "$OUT")" \
      "*** success is NOT claimed ***"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" "exits non-zero"
check "$([ ! -f "$T/cluster/cluster.pids" ] && echo yes || echo no)" \
      "...and leaves no pid file pointing at dead processes"

# ═══ 6: a live cluster is not double-started ═════════════════════════════
echo
echo "-- 6: refusing to start on top of a running cluster --"
export STUB_ARGV_LOG="$T/argv6.txt"; : > "$STUB_ARGV_LOG"
launch_bg 2 --nodes 2
OUT="$(run timeout 20 ./run-cluster.sh --nodes 2)"; RC=$?
check "$(has 'still up' "$OUT")" "*** a second launch is refused while nodes run ***"
check "$(has -- '--stop' "$OUT")" "...and says how to clear it"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" "exits non-zero"
run timeout 20 ./run-cluster.sh --stop >/dev/null; launch_wait_exit

# ═══ 7: headless, and a busy console port ════════════════════════════════
echo
echo "-- 7: environment --"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 2 --dry-run)"
check "$(has 'display          none' "$OUT")" \
      "*** headless picks -display none, not the gtk that fails ***"

if command -v python3 >/dev/null 2>&1; then
    python3 -c "
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',12342)); s.listen(1); time.sleep(6)" 2>/dev/null &
    HOLDER=$!
    sleep 1
    OUT="$(run timeout 20 ./run-cluster.sh --nodes 3)"
    kill "$HOLDER" 2>/dev/null || true
    check "$(has 'console port 12342' "$OUT")" "a held console port is named"
    check "$(has 'node 2' "$OUT")" "...along with whose it is"
    check "$(hasnt 'Building the kernel' "$OUT")" \
          "*** ...and caught BEFORE building anything ***"
fi

# ═══ 8: capacity sizing ══════════════════════════════════════════════════
# Pure arithmetic over detected inputs, which is exactly why it is testable
# without a big machine: the detection is overridable, so a 4-core/15 GiB
# server can be modelled here.
echo
echo "-- 8: host capacity and --nodes auto --"
SERVER="AEROSLS_HOST_CORES=4 AEROSLS_HOST_MEM_MB=15000 AEROSLS_HOST_DISK_MB=109000"

OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_CORES=4 \
      AEROSLS_HOST_MEM_MB=15000 AEROSLS_HOST_DISK_MB=109000 \
      timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'auto -> 8' "$OUT")"       "*** a 4-core/15 GiB host sizes to 8 nodes ***"
check "$(has 'bound by the roster cap' "$OUT")"       "*** ...bound by CLUSTER_NODE_MAX, not by the hardware ***"
check "$(has 'by memory          12' "$OUT")" "the memory bound is shown (12)"
check "$(has 'CPU (advisory)' "$OUT")"       "*** CPU is ADVISORY, not a hard bound -- idle nodes halt ***"
check "$(has '3 busy nodes' "$OUT")"       "...and says how many could be busy at once, which is the real risk"

# Memory binding rather than the roster cap.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_MEM_MB=6000 AEROSLS_HOST_DISK_MB=109000 \
      timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'auto -> 3' "$OUT")" "a 6 GiB host sizes to 3 at 1 GiB each"
check "$(has 'bound by memory' "$OUT")" "...and names memory as the binding constraint"

# Disk binding -- the one an operator would not guess.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_MEM_MB=64000 AEROSLS_HOST_DISK_MB=25000 \
      timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'auto -> 2' "$OUT")" "plenty of RAM but little disk sizes to 2"
check "$(has 'bound by free disk' "$OUT")" "...and names disk"

# An explicit count over capacity is refused, not clamped.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_MEM_MB=6000 AEROSLS_HOST_DISK_MB=109000 \
      timeout 20 ./run-cluster.sh --nodes 8 --dry-run 2>&1)"; RC=$?
check "$(has 'exceeds this host' "$OUT")"       "*** an explicit 8 on a 3-node host is REFUSED ***"
check "$(hasnt 'nodes            8' "$OUT")"       "*** ...not silently clamped to 3, which would waste an afternoon ***"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" "exits non-zero"

OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_MEM_MB=6000 AEROSLS_HOST_DISK_MB=109000 \
      timeout 20 ./run-cluster.sh --nodes 8 --force --dry-run 2>&1)"
check "$(has 'nodes            8' "$OUT")" "--force proceeds anyway"

# A host too small for even one node must blame the host, not the roster.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_MEM_MB=2500 \
      AEROSLS_HOST_DISK_MB=109000 timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'room for 0 nodes' "$OUT")" "a host too small says so plainly"
check "$(hasnt 'CLUSTER_NODE_MAX' "$OUT")"       "*** ...and does NOT blame the roster cap, which would send you to the ""wrong file ***"
check "$(has 'would fit at --ram 452M' "$OUT")"       "*** ...it computes a size that WOULD work ***"

# ...and that suggestion has to be true.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_MEM_MB=2500 \
      AEROSLS_HOST_DISK_MB=109000 timeout 20 ./run-cluster.sh --nodes auto --ram 452M --dry-run 2>&1)"
check "$(has 'auto -> 1' "$OUT")"       "*** ...and taking the suggestion actually works ***"

# ═══ 8b: detection itself, not just the arithmetic over it ═══════════════
# The overrides above short-circuit detect_mem_mb() entirely, so without
# this the one line that must read MemAvailable rather than MemTotal was
# never executed -- and a mutation swapping them survived the whole suite.
echo
echo "-- 8b: reading the host's real figures --"
cat > "$T/meminfo.fake" <<'MEMEOF'
MemTotal:       32000000 kB
MemFree:          500000 kB
MemAvailable:    6000000 kB
Buffers:          100000 kB
MEMEOF
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY -u AEROSLS_HOST_MEM_MB \
      AEROSLS_MEMINFO="$T/meminfo.fake" AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_DISK_MB=200000 timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'memory available   5859' "$OUT")"       "*** MemAvailable is read, not MemTotal -- 5859 MiB, not 30517 ***"
check "$(has 'auto -> 3' "$OUT")"       "*** ...so it sizes to 3, not the 29 MemTotal would have allowed ***"

# The reserve is subtracted, not ignored.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY -u AEROSLS_HOST_MEM_MB \
      AEROSLS_MEMINFO="$T/meminfo.fake" AEROSLS_HOST_CORES=8 \
      AEROSLS_HOST_DISK_MB=200000 AEROSLS_HOST_RESERVE_MB=0 \
      timeout 20 ./run-cluster.sh --nodes auto --dry-run 2>&1)"
check "$(has 'auto -> 5' "$OUT")"       "*** dropping the host reserve to 0 raises it 3 -> 5, so it IS subtracted ***"

# ═══ 9: --ram parsing and the floor ══════════════════════════════════════
echo
echo "-- 9: --ram --"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 1 --ram 200M --dry-run)"
check "$(has 'below the 256M floor' "$OUT")"       "*** below the floor is refused -- the kernel image alone is ~120 MiB ***"
check "$(has '_kernel_image_end' "$OUT")" "...naming where the floor comes from"
OUT="$(run timeout 20 ./run-cluster.sh --nodes 1 --ram 1.5G --dry-run)"
check "$(has 'not a size I understand' "$OUT")"       "a size that cannot be parsed is refused, not guessed at"
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_HOST_MEM_MB=64000 \
      AEROSLS_HOST_DISK_MB=109000 timeout 20 ./run-cluster.sh --nodes 1 --ram 2G --dry-run 2>&1)"
check "$(has '2048 MiB / 1 vCPU' "$OUT")" "2G is read as 2048 MiB"

# ═══ 10: inherited from run_two_nodes_harness.sh ═════════════════════════
# That harness is retired (run-two-nodes.sh with it), so its checks that
# had no equivalent here move rather than disappear. Retiring a script by
# dropping its tests is how a regression gets in quietly.
echo
echo "-- 10: carried over from the retired two-node harness --"

# The failure originally reported from the server: gtk forced on a host
# with no display. Scenario 5 covers partial failure via STUB_FAIL_NODE;
# this covers the real-world cause, where EVERY node dies of the same thing.
OUT="$(cd "$T" && env -u DISPLAY -u WAYLAND_DISPLAY AEROSLS_DISPLAY=gtk \
      timeout 25 ./run-cluster.sh --nodes 3 2>&1)"; RC=$?
check "$(has 'gtk initialization failed' "$OUT")"       "*** the originally reported failure: QEMU's own stderr is surfaced ***"
check "$(has '3 of 3 node' "$OUT")"       "*** ...and ALL three are reported, not just the first ***"
check "$(hasnt 'nodes up and confirmed running' "$OUT")" "success is not claimed"
check "$([ "$RC" -ne 0 ] && echo yes || echo no)" "exits non-zero"

# An explicit backend beats autodetection, even where one would be found.
OUT="$(cd "$T" && DISPLAY=:0 AEROSLS_DISPLAY=none timeout 20 \
      ./run-cluster.sh --nodes 2 --dry-run 2>&1)"
check "$(has 'display          none' "$OUT")"       "AEROSLS_DISPLAY overrides autodetection even when DISPLAY is set"

# The segment port is bound by EVERY node on purpose -- that shared bind is
# the segment. Treating it as a clash would refuse a launch that is working
# exactly as designed.
if command -v python3 >/dev/null 2>&1; then
    python3 -c "
import socket,time
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('',12340)); time.sleep(8)" 2>/dev/null &
    HOLDER=$!
    sleep 1
    launch_bg 2 --nodes 2
    OUT="$(cat "$T/launch.out")"
    kill "$HOLDER" 2>/dev/null || true
    check "$(hasnt 'already in use' "$OUT")"           "*** something already on the segment port does NOT block a launch ***"
    check "$(has '2 nodes up and confirmed running' "$OUT")" "...the cluster forms"
    run timeout 20 ./run-cluster.sh --stop >/dev/null; launch_wait_exit
fi

echo
echo "=========================================="
echo "passed=$passed failed=$failed"
[ "$failed" -eq 0 ]

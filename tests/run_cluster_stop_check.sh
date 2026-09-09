#!/usr/bin/env bash
# tests/run_cluster_stop_check.sh — run-cluster.sh --stop actually stops things,
# and says so only when it did.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# --stop used to send SIGTERM and report success on the strength of `kill`
# returning 0. That return means the signal was DELIVERED. It says nothing
# about whether anything died — a process with `trap "" TERM` scores exactly
# the same as one that exited. The pid file was then removed unconditionally,
# survivors included, so the one record of what was still running was gone and
# a second --stop answered "no cluster to stop" while the nodes were up.
#
# On the deploy host that failure had a second half. pm2 supervises the kernel,
# so SIGTERM does kill the process — and pm2 immediately starts a new one under
# a NEW pid. Every pid --stop knew about is genuinely dead, the loop is entirely
# correct, and the cluster never goes down. No pid-based check can see this,
# which is why the script re-probes the HTTP ports afterwards.
#
# Four behaviours, each with a process built to exhibit it:
#
#   1. a normal process           -> dies on SIGTERM, reported "stopped"
#   2. `trap "" TERM`             -> escalated to SIGKILL, reported "killed"
#   3. a pid that no longer exists-> reported "was not running", not "stopped"
#   4. a listener that reappears  -> WARNING naming pm2, exit 1
#
# Case 4 is the one that matters and the one a pid-based test cannot express,
# so it is built the way the real thing behaves: the process is allowed to die
# cleanly, and something else is listening on its port a moment later.
#
# Runs against a COPY of run-cluster.sh in a scratch directory, never the repo
# checkout, so a bug in the script under test cannot kill a real cluster or
# delete a real pid file.
#
# Exit: 0 pass, 1 fail, 2 skip (no bash /dev/tcp, or no python3 for the
# listener — a check that quietly tests three cases instead of four would
# report the same PASS for less coverage).
set -u
cd "$(dirname "$0")/.."

SCRIPT="run-cluster.sh"
[ -f "$SCRIPT" ] || { echo "ABORT: $SCRIPT not found."; exit 2; }
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not found; case 4 needs it to hold a port open."; exit 2; }

# The port probe is a bash builtin that can be compiled out (--disable-net-
# redirections, as in some distro /bin/sh builds). If this bash lacks it the
# script's own probe is inert too, so testing it would prove nothing.
if ! (exec 3<>/dev/tcp/127.0.0.1/1) 2>/dev/null; then
    if ! echo "$BASH_VERSION" >/dev/null 2>&1; then
        echo "ABORT: not bash."; exit 2
    fi
fi
# A closed port refusing the connection is the expected outcome above; what we
# cannot tolerate is /dev/tcp being unsupported, which reports the same way.
# Distinguish by connecting to something that IS listening.
probe_supported=0
python3 - <<'PY' >/tmp/rcs_probe_port 2>/dev/null &
import socket, time
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)
print(s.getsockname()[1], flush=True)
time.sleep(5)
PY
probe_pid=$!
for _ in $(seq 1 30); do [ -s /tmp/rcs_probe_port ] && break; sleep 0.1; done
pport="$(cat /tmp/rcs_probe_port 2>/dev/null || true)"
# Bounded probe: on a host where connect() to a closed loopback port hangs
# (WSL2 mirrored networking does this after its loopback state goes stale),
# an unbounded /dev/tcp probe hangs --stop's own re-probe AND this check's
# listener waits. 2s per probe is far above a healthy refusal (<1ms) and
# keeps the check fast everywhere.
bash_closed_probe() {
    local rc=0
    timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$1" 2>/dev/null || rc=$?
    return "$rc"
}
if [ -n "$pport" ] && bash_closed_probe "$pport"; then
    probe_supported=1
fi
kill "$probe_pid" 2>/dev/null || true; wait "$probe_pid" 2>/dev/null || true
rm -f /tmp/rcs_probe_port
if [ "$probe_supported" -ne 1 ]; then
    echo "ABORT: this bash has no /dev/tcp; the port re-probe cannot be tested."
    exit 2
fi

WORK="$(mktemp -d)"
cleanup() {
    # Anything the cases left behind. Losing a stray `sleep 300` on a dev box is
    # untidy; leaving one on a build agent is a slow leak of that agent -- and
    # worse than untidy here, see the /dev/null note at each spawn.
    for p in ${SPAWNED:-}; do kill -KILL "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT
SPAWNED=""

cp "$SCRIPT" "$WORK/"
mkdir -p "$WORK/net" "$WORK/cluster"
# --stop runs after the CLUSTER_NODE_MAX read, so this only silences a warning
# that would otherwise land in the middle of the output being asserted on.
cp net/consensus.h "$WORK/net/" 2>/dev/null || \
    echo "#define CLUSTER_NODE_MAX 8" > "$WORK/net/consensus.h"

fails=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }

# Asserts $2 appears in the captured output $1, printing the output when it
# does not — a failure that only says "expected X" makes you re-run by hand.
expect() {
    local out="$1" want="$2" what="$3"
    case "$out" in
        *"$want"*) pass "$what" ;;
        *) fail "$what -- expected to see: $want"
           printf '%s\n' "$out" | sed 's/^/        | /' ;;
    esac
}
refute() {
    local out="$1" unwanted="$2" what="$3"
    case "$out" in
        *"$unwanted"*) fail "$what -- should NOT have said: $unwanted"
                       printf '%s\n' "$out" | sed 's/^/        | /' ;;
        *) pass "$what" ;;
    esac
}

# ─── Case 0: nothing to stop ───────────────────────────────────────────────
out="$(cd "$WORK" && ./run-cluster.sh --stop 2>&1)"; rc=$?
expect "$out" "no cluster to stop" "no pid file: says so"
[ "$rc" -eq 0 ] && pass "no pid file: exit 0" || fail "no pid file: exit $rc, want 0"

# ─── Cases 1-3: clean exit, TERM-ignoring, already dead ────────────────────
# Every spawned process gets /dev/null for stdout and stderr, which is not
# tidiness. A background process inherits this script's stdout, so if the check
# is run through a pipe -- `... | tail`, or the capture run_checks.sh does --
# any survivor holds the write end open and the reader never sees EOF. The
# check passes, prints everything, exits 0, and the pipeline hangs anyway.
sleep 300 >/dev/null 2>&1 & normal=$!
bash -c 'trap "" TERM; sleep 300' >/dev/null 2>&1 & stubborn=$!
sleep 300 >/dev/null 2>&1 & doomed=$!

# `bash -c '...; sleep 300'` runs the sleep as a CHILD of that bash, so
# SIGKILLing the bash orphans a sleep with 300 seconds left. It is the one
# survivor this check reliably creates, so it is tracked explicitly rather than
# left to init. (Making the stubborn process childless would be neater, but a
# TERM-ignoring shell needs a trap, and a trap needs a shell that is still
# running something interruptible.)
sleep 0.3
stubborn_child="$(pgrep -P "$stubborn" 2>/dev/null || true)"
[ -z "$stubborn_child" ] && stubborn_child="$(ps -o pid= --ppid "$stubborn" 2>/dev/null | tr -d ' ' || true)"

SPAWNED="$normal $stubborn $stubborn_child $doomed"
# Off the job table, so bash does not print its own "Killed" notification into
# the middle of the assertions below when --stop SIGKILLs it.
disown "$stubborn" 2>/dev/null || true
kill -KILL "$doomed" 2>/dev/null; wait "$doomed" 2>/dev/null || true

printf '1 %s\n2 %s\n3 %s\n' "$normal" "$stubborn" "$doomed" > "$WORK/cluster/cluster.pids"

# AEROSLS_HTTP_BASE is pushed into a range nothing here listens on, so the
# post-stop probe finds every port closed and cases 1-3 are judged on the
# kill path alone. Case 4 sets it back deliberately.
out="$(cd "$WORK" && AEROSLS_HTTP_BASE=59990 ./run-cluster.sh --stop 2>&1)"; rc=$?

expect "$out" "stopped node 1 (pid $normal)"       "case 1: SIGTERM-able process reported stopped"
expect "$out" "ignored SIGTERM"                     "case 2: escalation is announced, not silent"
expect "$out" "killed node 2 (pid $stubborn)"       "case 2: SIGKILL escalation reported"
expect "$out" "node 3 (pid $doomed) was not running" "case 3: dead pid distinguished from stopped"
refute "$out" "stopped node 3"                       "case 3: dead pid not counted as a stop"
expect "$out" "stopped 1, killed 1, already gone 1"  "tally matches the three outcomes"
[ "$rc" -eq 0 ] && pass "all stopped: exit 0" || fail "all stopped: exit $rc, want 0"

kill -0 "$normal"   2>/dev/null && fail "case 1: process survived --stop"   || pass "case 1: process is gone"
kill -0 "$stubborn" 2>/dev/null && fail "case 2: process survived SIGKILL"  || pass "case 2: process is gone"
[ -f "$WORK/cluster/cluster.pids" ] && fail "pid file kept after a full stop" || pass "pid file removed after a full stop"

# ─── Case 4: the pm2 shape — pid dies, port comes back ─────────────────────
# The listener is started BEFORE --stop and outlives it, which is what a
# supervisor's replacement process looks like from the outside: the pid in the
# file is dead and the port is served. Reproducing pm2 itself would test pm2.
port=59123
python3 - "$port" <<'PY' >/dev/null 2>&1 &
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
time.sleep(60)
PY
listener=$!
SPAWNED="$SPAWNED $listener"
for _ in $(seq 1 40); do
    bash_closed_probe "$port" && break
    sleep 0.1
done

sleep 300 >/dev/null 2>&1 & victim=$!
SPAWNED="$SPAWNED $victim"
printf '3 %s\n' "$victim" > "$WORK/cluster/cluster.pids"

# HTTP_BASE + 3 == 59123, the port the listener holds.
out="$(cd "$WORK" && AEROSLS_HTTP_BASE=59120 ./run-cluster.sh --stop 2>&1)"; rc=$?

expect "$out" "stopped node 3 (pid $victim)" "case 4: the pid itself did stop"
expect "$out" "WARNING"                      "case 4: respawn detected"
expect "$out" "pm2"                          "case 4: names the likely supervisor"
expect "$out" "pm2 stop"                     "case 4: gives the command that works"
[ "$rc" -eq 1 ] && pass "case 4: exit 1 (the stop did not achieve its goal)" \
                || fail "case 4: exit $rc, want 1 -- a respawned cluster is not a successful stop"

kill "$listener" 2>/dev/null || true

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS  run-cluster.sh --stop: verifies death, escalates, and detects respawn"
    exit 0
fi
echo "FAIL  $fails check(s) failed"
exit 1

#!/usr/bin/env bash
# tests/free_port.sh — print the first free loopback TCP port in the guard
# band (AEROSLS_FREE_PORT_RANGE, default 32001-32020) for a boot check's QEMU
# hostfwd, RESERVED until QEMU binds it — or, with --print-band, the band
# itself without probing, or, with --lock-file <port>, the reservation file
# that port's reservation lives in.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# run-cluster.sh gives node i its REST API on host port 3000+i, and the deploy
# gate (deploy/deploy.sh) runs tests/run_checks.sh while the live instance
# still serves. Boot checks that hard-coded their hostfwd port therefore
# collided with live nodes: on the 2026-09-13 deploy, a cluster held
# 3001..3003, so partition_teardown_boot_check (3002) and
# simi_teardown_boot_check (3003) could not start QEMU at all — "Could not set
# up host forwarding rule" went to /dev/null and surfaced only as "QEMU exited
# before the grub menu appeared" — while aerocap (3011) and tcache_roundtrip
# (which already scanned for a free port) stayed green. The same class of
# collision as the 2026-08-18 storage-image flock failure, on the port.
#
# A taken port must fail to bind, not silently test the wrong service, so
# probe first and hand back one nothing answers on.
#
# ─── The band rule: never inside the live REST band ────────────────────────
# THE DEFAULT USED TO BE 3001..3020, WHICH IS THE LIVE BAND. Probing protects
# a port something ANSWERS on. It cannot protect a port something else
# TRUSTS — and three things trust 3001 without probing:
#   * tests/webapp_served_check.sh, whose default URL is http://localhost:3001
#     (the production backend) and which cannot tell a deployed kernel from a
#     boot-check kernel: both answer /api/health;
#   * the inline QEMU steps in .github/workflows/ci.yml (health smoke, decoder
#     fixtures), which gated on a fixed :3001;
#   * make x86-run (X86_HTTP_PORT, default 3001 — the host port the README and
#     docs/COMMANDS.md tell a human to curl).
# On 2026-09-24 that is exactly what went wrong: a local boot check probed
# 3001, found nothing answering, booted its own kernel there, and the CI
# webapp_served_check.sh running on the same host compared THAT kernel's
# assets against the committed bundle — a red 53-minute job whose cause was
# nowhere in the kernel, the guard, or the commit. The port it handed out was
# production's, because the band it scanned was production's.
#
# So the guard band is now disjoint from the live band by construction, and a
# band that overlaps is REFUSED here rather than warned about:
#     live band = AEROSLS_HTTP_BASE+1 .. AEROSLS_HTTP_BASE+CLUSTER_NODE_MAX
#                 (node i's REST API; node cap read from net/consensus.h),
#                 plus the production backend on 3001.
# A host that runs the CI runner, or the live cluster, and also wants to run
# boot checks locally moves itself out of the way with
# AEROSLS_FREE_PORT_RANGE=<start>-<end>. Nothing else needs to change: every
# tester of a QEMU hostfwd port goes through this allocator.
#
# ─── The reservation: a probe is a snapshot, not a claim ───────────────────
# The probe answers "is anything answering on p right now?", not "will p still
# be free when my QEMU calls bind()". Between those two instants another guard
# run can probe the same port, find it free too, and start its own QEMU: one of
# the two bind()s then fails and that QEMU exits — the 2026-09-13 shape, and
# two runs of tests/run_checks.sh in parallel are enough to reach it, because
# both of them scan the same band from the bottom.
#
# So an allocation is RESERVED, not merely probed. Per port there is a lock
# file (AEROSLS_FREE_PORT_LOCK_DIR, default /tmp/aerosls-guard-ports-<uid>/)
# which the allocator flocks non-blockingly BEFORE probing the port; a port
# whose lock is already held is skipped WITHOUT being probed, so two
# concurrent runs cannot both pass the probe and then collide on the bind.
# The lock is then kept by a detached holder subshell (it outlives this script,
# which exits as soon as it has printed the port) until either
#
#   * the port becomes bound — QEMU has taken it, which is exactly what the
#     reservation was for. The occupancy test reads the kernel's socket table
#     (/proc/net/tcp), so the holder never connect()s to the guest's HTTP
#     server while it waits; or
#   * AEROSLS_FREE_PORT_HOLD_S expires (default 300) — so a guard killed
#     between allocating and launching QEMU cannot hold a port out of the band
#     for good.
#
# What this does NOT do: a squatter that does not cooperate (anything that
# binds the port without taking the lock) is still only caught by the probe,
# and QEMU's own bind still fails loudly if one slips in during the window that
# remains. The reservation narrows that window from "a whole guard run" to "the
# microseconds between the lock and the bind", it does not remove it.
# AEROSLS_FREE_PORT_HOLD=0 restores probe-only allocation, for a host without
# util-linux's flock and for tests that must not leave a reservation behind;
# the boot checks and the CI steps never set it.
#
# The lock is advisory and per uid, so it excludes other guard runs of the same
# user on this host — the CI runner's steps, and two local runs — which is the
# collision that actually happens. It is not a security boundary and does not
# pretend to be one.
#
# Each probe is bounded (timeout 2): on a host where connect() to a closed
# loopback port hangs instead of refusing — observed on WSL2 with
# networkingMode=mirrored after its loopback state goes stale — an unbounded
# probe hangs the port scan forever. A healthy refusal is <1ms, so 2s never
# changes the answer on a well host; a hung probe is treated as free, and a
# genuinely taken port still makes QEMU's bind fail loudly. A hung probe cannot
# hand the same port to two runs: the lock, not the probe, is what makes the
# allocation mutually exclusive.
#
# Callers should bind the port on 127.0.0.1 (hostfwd=tcp:127.0.0.1:$PORT-:3000)
# and talk to http://127.0.0.1:$PORT — the probe only proves loopback is free,
# and a deploy host must not expose a test kernel on a public interface.
#
# Environment:
#   AEROSLS_FREE_PORT_RANGE  the band, START-END or a single port
#                            (default 32001-32020 — outside the live band)
#   AEROSLS_HTTP_BASE        live REST port base (default 3000); used only to
#                            refuse a band that overlaps the live band
#   AEROSLS_FREE_PORT_HOLD   1 (default) reserve the port until QEMU binds it;
#                            0 probe only (tests that must not leave a
#                            reservation behind, hosts without flock)
#   AEROSLS_FREE_PORT_HOLD_S seconds a reservation may outlive the allocating
#                            script (default 300, 0.2s granularity)
#   AEROSLS_FREE_PORT_LOCK_DIR where the per-port lock files live (default
#                            /tmp/aerosls-guard-ports-<uid>; flock needs a
#                            local filesystem, so point this at one if TMPDIR
#                            is not)
#
# Usage:
#   PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
#   BAND=$(bash tests/free_port.sh --print-band)        # the resolved band, no probe
#   LOCK=$(bash tests/free_port.sh --lock-file "$PORT")  # that port's reservation file
#
# Exit: 0 with the port (or band, or lock path) on stdout; 1 if the band is
# unusable — every port taken, a malformed range, a range that overlaps the
# live REST band, or a reservation that could not be taken (no flock).
set -u

# The tests/ directory, from this file's own path: callers run this file from
# the repo root, and a few (the checks) from a scratch tree.
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PRINT_BAND=0
LOCK_FILE_FOR=""
case "${1:-}" in
    --print-band) PRINT_BAND=1 ;;
    --lock-file)
        LOCK_FILE_FOR="${2:-}"
        case "$LOCK_FILE_FOR" in
            ''|*[!0-9]*) echo "free_port: --lock-file needs a port number" >&2; exit 1 ;;
        esac
        ;;
    "") ;;
    -h|--help) sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "free_port: unknown option '$1'" >&2; exit 1 ;;
esac

RANGE="${AEROSLS_FREE_PORT_RANGE:-32001-32020}"

# START-END, or a single port (END=START). Anything else is refused here: a
# silently-ignored bad range would hand QEMU a port nobody asked for.
case "$RANGE" in
    *-*) START="${RANGE%%-*}"; END="${RANGE#*-}" ;;
    *)   START="$RANGE";       END="$RANGE" ;;
esac
case "$START$END" in
    ''|*[!0-9]*)
        echo "ABORT: AEROSLS_FREE_PORT_RANGE='$RANGE' is not START-END (or a single port) — e.g. 32001-32020." >&2
        exit 1
        ;;
esac
if [ "$START" -lt 1 ] || [ "$END" -gt 65535 ] || [ "$START" -gt "$END" ]; then
    echo "ABORT: AEROSLS_FREE_PORT_RANGE='$RANGE' is out of range (need 1 <= START <= END <= 65535)." >&2
    exit 1
fi

# ─── The live band, which the guard band must not touch ────────────────────
HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
case "$HTTP_BASE" in ''|*[!0-9]*) HTTP_BASE=3000 ;; esac
NODE_MAX="$(sed -n 's/^#define CLUSTER_NODE_MAX[[:space:]]\+\([0-9]\+\).*/\1/p' "$SRC_DIR/../net/consensus.h" 2>/dev/null | head -1)"
case "${NODE_MAX:-}" in ''|*[!0-9]*) NODE_MAX=8 ;; esac
LIVE_LO=$((HTTP_BASE + 1))
LIVE_HI=$((HTTP_BASE + NODE_MAX))
PROD=3001   # the production backend (Caddy -> localhost:3001), whatever the base

overlaps() {
    # $1..$2 the band, $3..$4 the range being tested against
    [ "$1" -le "$4" ] && [ "$3" -le "$2" ]
}

if overlaps "$START" "$END" "$LIVE_LO" "$LIVE_HI" || { [ "$START" -le "$PROD" ] && [ "$PROD" -le "$END" ]; }; then
    echo "ABORT: AEROSLS_FREE_PORT_RANGE='$RANGE' overlaps the live REST band." >&2
    echo "       Live: nodes $LIVE_LO..$LIVE_HI (AEROSLS_HTTP_BASE=$HTTP_BASE + i, i <= $NODE_MAX)" >&2
    echo "       and the production backend on $PROD." >&2
    echo "       A QEMU boot check on a live port does not fail — it makes whatever" >&2
    echo "       trusts that port without probing test the wrong machine (see this" >&2
    echo "       file's header). Pick a band outside it, e.g. 32001-32020." >&2
    exit 1
fi

# ─── The reservation knobs ────────────────────────────────────────────────
HOLD="${AEROSLS_FREE_PORT_HOLD:-1}"
case "$HOLD" in
    0|1) ;;
    *)
        echo "ABORT: AEROSLS_FREE_PORT_HOLD='$HOLD' is not 0 or 1 — 1 reserves the port" >&2
        echo "       until QEMU binds it, 0 is probe-only allocation." >&2
        exit 1
        ;;
esac
HOLD_S="${AEROSLS_FREE_PORT_HOLD_S:-300}"
case "$HOLD_S" in
    ''|*[!0-9]*)
        echo "ABORT: AEROSLS_FREE_PORT_HOLD_S='$HOLD_S' is not a whole number of seconds." >&2
        exit 1
        ;;
esac
LOCK_DIR="${AEROSLS_FREE_PORT_LOCK_DIR:-${TMPDIR:-/tmp}/aerosls-guard-ports-${UID:-0}}"

lock_file() { printf '%s/port-%s.lock\n' "$LOCK_DIR" "$1"; }

if [ -n "$LOCK_FILE_FOR" ]; then
    lock_file "$LOCK_FILE_FOR"
    exit 0
fi

if [ "$PRINT_BAND" = "1" ]; then
    echo "$START-$END"
    exit 0
fi

if [ "$HOLD" = "1" ]; then
    # Refuse rather than silently degrade: without flock this file cannot keep
    # the promise its callers rely on, and probing alone is what let two runs
    # collide (see the header).
    command -v flock >/dev/null 2>&1 || {
        echo "ABORT: flock not found, so a guard port cannot be reserved until QEMU" >&2
        echo "       binds it: two concurrent guard runs could both pass the probe and" >&2
        echo "       then collide on the bind. flock ships with util-linux. Install it," >&2
        echo "       or accept probe-only allocation with AEROSLS_FREE_PORT_HOLD=0." >&2
        exit 1
    }
    mkdir -p "$LOCK_DIR" 2>/dev/null || {
        echo "ABORT: cannot create the guard port lock directory $LOCK_DIR." >&2
        echo "       Point AEROSLS_FREE_PORT_LOCK_DIR somewhere writable, or accept" >&2
        echo "       probe-only allocation with AEROSLS_FREE_PORT_HOLD=0." >&2
        exit 1
    }
fi

# Is the port bound by anything (listening) right now? Reads the kernel's own
# socket table rather than connect()ing: the reservation holder calls this
# while QEMU is up, and a probe would open a real connection to the guest's
# HTTP server every time. The bounded probe is the fallback where /proc is not
# readable.
port_is_bound() {
    local port=$1 phex
    if [ -r /proc/net/tcp ]; then
        phex="$(printf '%04X' "$port")"
        local files=(/proc/net/tcp)
        [ -r /proc/net/tcp6 ] && files+=(/proc/net/tcp6)
        # field 2 is local_address:port, field 4 the state (0A = LISTEN).
        awk -v want="$phex" '
            NR > 1 { split($2, addr, ":"); if ($4 == "0A" && addr[2] == want) found = 1 }
            END { exit found ? 0 : 1 }
        ' "${files[@]}" 2>/dev/null
        return
    fi
    timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$port" 2>/dev/null
}

# Take the port's reservation: hold its lock file open+locked. Non-blocking, so
# a port another run has reserved is skipped rather than waited for. The fd is
# deliberately left open — it is what carries the lock to the holder.
HOLD_FD=""
reserve_port() {
    local p=$1 fd
    exec {fd}>>"$(lock_file "$p")" 2>/dev/null || return 1
    if ! flock -n "$fd" 2>/dev/null; then
        exec {fd}>&- 2>/dev/null || true
        return 1
    fi
    HOLD_FD="$fd"
    return 0
}

release_port() {
    [ -n "${HOLD_FD:-}" ] || return 0
    exec {HOLD_FD}>&- 2>/dev/null || true
    HOLD_FD=""
}

# The holder. Runs detached (started with &, stdout/stderr on /dev/null so the
# caller's command substitution still sees EOF), inherits the locked fd, and
# returns once the reservation has served its purpose — the port is bound — or
# once its TTL is up. Never connects to the guest.
hold_until_bound() {
    local port=$1 ticks=$(( $2 * 5 ))   # 0.2s granularity
    while [ "$ticks" -gt 0 ]; do
        port_is_bound "$port" && return 0
        sleep 0.2
        ticks=$((ticks - 1))
    done
    return 0
}

for p in $(seq "$START" "$END"); do
    # Reservation before probing: a port another run has reserved is skipped
    # without being touched, so its QEMU and ours can never meet on the bind.
    if [ "$HOLD" = "1" ]; then
        reserve_port "$p" || continue
    fi
    if ! timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$p" 2>/dev/null; then
        if [ "$HOLD" = "1" ]; then
            hold_until_bound "$p" "$HOLD_S" >/dev/null 2>&1 &
        fi
        echo "$p"
        exit 0
    fi
    if [ "$HOLD" = "1" ]; then
        release_port
    fi
done

echo "ABORT: no free loopback port in $RANGE — every port in the guard band is" >&2
echo "       taken or reserved by another guard run. Widen or move the band with" >&2
echo "       AEROSLS_FREE_PORT_RANGE=<start>-<end>." >&2
exit 1

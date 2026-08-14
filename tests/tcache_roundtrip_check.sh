#!/usr/bin/env bash
# tests/tcache_roundtrip_check.sh — the Phase 2 persistent translation cache
# must survive a reboot, on the decoder build, every time it is measured.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The Phase 2 headline (plan §3b-3e, 2026-08-04) — 8 blocks compiled on a
# cold boot, 0 after a checkpoint + reboot, identical guest result — was
# measured by hand on a cluster node. It predates the M8 flip, and the
# re-measurement on the decoder build (iteration 32) was ALSO by hand. A
# property that is only verified when someone remembers to measure it rots
# the day a change makes the round-trip silently not happen: the guard
# passes nothing and nobody notices, until a warm boot recompiles the world
# and the "compiled code survives reboot" claim quietly dies.
#
# This guard is the machine: boot, bench (everything compiled), checkpoint
# (TBs to NVMe), reboot, bench again (everything hit, nothing compiled),
# assert the invariant. It runs on every kernel-guards CI push and in the
# deploy gate, so a regression in the chain fails a build instead of
# waiting for someone to re-measure.
#
# ─── What it asserts, and why in invariant form ────────────────────────────
# The plan's "8/8 hits, 0 blocks compiled, identical 502 insns" is asserted
# as the invariant it really is, not as magic numbers:
#
#   cold.json:  ok=true, cold=true, tcache_hits=0, blocks>=1, insns>=1
#   checkpoint: status=0, and the log shows "synced: N TBs"
#   boot 1 log: "no snapshot" (NVMe up AND the disk was fresh), and NOT
#               "NVMe unavailable" (the >4 GiB BAR degradation, below)
#   boot 2 log: "warm start" (the cache was restored from NVMe)
#   warm.json:  ok=true, cold=false, blocks=0, tcache_misses=0,
#               tcache_hits == cold blocks, insns == cold insns
#
# warm_hits == cold_blocks is the round-trip contract: every block compiled
# on the cold boot must be a hit on the warm boot, with nothing new
# compiled and the same instruction count. If TCG changes the block count
# (8 -> 9), the guard follows the kernel instead of false-failing.
#
# ─── RAM requirement (a real pitfall, not a footnote) ─────────────────────
# Must run with -m 1G, the cluster default. The NVMe device's 64-bit BAR
# lands ABOVE 4 GiB at higher RAM — outside the kernel's identity map — and
# the whole persistence stack (tcache, stream, persist) then honestly cold-
# starts with "[NVME] MMIO above 4 GiB". It is a loud message, not a silent
# degradation, but a QEMU harness at 4G would make every run here a
# no-op-shaped pass. The guard therefore asserts the NVMe path actually
# came up: boot 1 must read "no snapshot", never "NVMe unavailable".
#
# ─── Replay mode (how the smoke proves the teeth bite) ────────────────────
# --replay DIR runs ONLY the validation half against recorded artifacts
# (cold.json, checkpoint.json, boot.log, warm.json) — no boot, no qemu, no
# ISO. tests/tcache_roundtrip_smoke.sh synthesizes a well-formed artifact
# set, mutates each assertion input, and asserts the guard fails on each —
# so a regression that makes the guard blind (a wrong field, a dropped
# grep) fails the smoke instead of passing authority.
#
# Exit: 0 pass, 1 fail, 2 abort (prerequisite missing).
# GUARD-KIND: build
set -u
cd "$(dirname "$0")/.."   # repo root

ISO="${TCACHE_ISO:-sls_operating_system.iso}"
PORT="${TCACHE_PORT:-}"
TOK="deadbeef01234567cafebabe76543210"   # dave, DB_ADMIN — kernel/auth.c

REPLAY=""
if [ "${1:-}" = "--replay" ]; then
    REPLAY="${2:-}"
    [ -d "$REPLAY" ] || { echo "ABORT: --replay dir '$REPLAY' not found" >&2; exit 2; }
fi

command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not found — the JSON assertions run in python." >&2
    exit 2
}

# ─── The validation half. Shared by the live run and --replay. ─────────────
# One python pass over all four artifacts; every assertion failure is a
# line. Missing or unparseable artifacts are failures too — a guard that
# could not read its inputs must not pass.
validate() {   # $1 = workdir
    python3 - "$1" <<'PYEOF'
import json, os, re, sys
W = sys.argv[1]
fails = []

def art(name):
    p = os.path.join(W, name)
    if not os.path.isfile(p):
        fails.append("missing artifact %s -- the round-trip never produced it" % name)
        return None
    return open(p, encoding="utf-8", errors="replace").read()

def jf(name):
    s = art(name)
    if s is None:
        return None
    try:
        return json.loads(s)
    except Exception as e:
        fails.append("%s: unparseable JSON (%s) -- a guard that cannot read its "
                     "inputs must not pass" % (name, e))
        return None

cold = jf("cold.json")
chk  = jf("checkpoint.json")
warm = jf("warm.json")
log  = art("boot.log")

if log is not None:
    if "NVMe unavailable" in log:
        fails.append("boot log: [QEMU-SLS TCACHE] NVMe unavailable -- the NVMe "
                     "BAR landed above 4 GiB (run with -m 1G); the persistence "
                     "stack cold-started and the round-trip never happened")
    if "no snapshot" not in log:
        fails.append("boot log: missing '[QEMU-SLS TCACHE] no snapshot' on boot 1 "
                     "-- a reused disk image would make the 'cold' half meaningless")
    if "warm start" not in log:
        fails.append("boot log: missing '[QEMU-SLS TCACHE] warm start' banner -- "
                     "the cache was not restored from NVMe after the reboot")
    if not re.search(r"QEMU-SLS TCACHE\] synced: [0-9]+ TBs", log):
        fails.append("boot log: missing 'synced: N TBs' line -- the checkpoint "
                     "never wrote the cache to NVMe")

if cold is not None:
    if cold.get("ok") != "true":
        fails.append("cold bench ok=%r -- expected true" % cold.get("ok"))
    if cold.get("cold") != "true":
        fails.append("cold bench cold=%r -- expected true: the first launch must "
                     "be a fresh cold start" % cold.get("cold"))
    if cold.get("tcache_hits") != 0:
        fails.append("cold bench tcache_hits=%r -- expected 0: a cold boot must "
                     "compile everything" % cold.get("tcache_hits"))
    cb = cold.get("blocks"); ci = cold.get("insns")
    if not isinstance(cb, int) or cb < 1:
        fails.append("cold bench blocks=%r -- expected >= 1 compiled" % cb)
    if not isinstance(ci, int) or ci < 1:
        fails.append("cold bench insns=%r -- expected >= 1" % ci)

if chk is not None and chk.get("status") != 0:
    fails.append("checkpoint status=%r -- expected 0 (the tcache sync to NVMe "
                 "failed)" % chk.get("status"))

if warm is not None and cold is not None:
    if warm.get("ok") != "true":
        fails.append("warm bench ok=%r -- expected true" % warm.get("ok"))
    if warm.get("cold") != "false":
        fails.append("warm bench cold=%r -- expected false: the warm run must "
                     "hit the restored cache, not recompile" % warm.get("cold"))
    if warm.get("blocks") != 0:
        fails.append("warm bench blocks=%r -- expected 0: nothing may be "
                     "compiled on the warm run" % warm.get("blocks"))
    if warm.get("tcache_misses") != 0:
        fails.append("warm bench tcache_misses=%r -- expected 0" % warm.get("tcache_misses"))
    if warm.get("tcache_hits") != cold.get("blocks"):
        fails.append("warm bench tcache_hits=%r != cold blocks=%r -- the restored "
                     "TBs were not all hit (the round-trip contract)"
                     % (warm.get("tcache_hits"), cold.get("blocks")))
    if warm.get("insns") != cold.get("insns"):
        fails.append("warm insns=%r != cold insns=%r -- the guest result differed "
                     "across the reboot" % (warm.get("insns"), cold.get("insns")))

print("\n".join(fails))
sys.exit(1 if fails else 0)
PYEOF
}

# ─── Replay mode: validate recorded artifacts, no boot. ────────────────────
if [ -n "$REPLAY" ]; then
    if out="$(validate "$REPLAY")"; then
        echo "PASS  replay: artifacts in $REPLAY satisfy the round-trip"
        exit 0
    fi
    echo "$out" | sed 's/^/      /'
    echo "FAIL  replay: artifacts in $REPLAY violate the round-trip"
    exit 1
fi

# ─── Live mode prerequisites. ──────────────────────────────────────────────
for tool in qemu-system-x86_64 qemu-img curl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ABORT: $tool not found — the live round-trip boots the ISO under QEMU." >&2
        exit 2
    }
done
[ -f "$ISO" ] || {
    echo "ABORT: $ISO not found — run 'make x86-iso' first." >&2
    exit 2
}

# Port. Default 3001; if it is taken (a live node on a deploy host, a
# previous boot), step up. A taken port must fail to bind, not silently
# test the wrong service.
if [ -z "$PORT" ]; then
    for p in $(seq 3001 3020); do
        if ! (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then
            PORT=$p
            break
        fi
    done
    [ -n "$PORT" ] || { echo "ABORT: no free port in 3001..3020" >&2; exit 2; }
fi

W="$(mktemp -d)"
IMG="$W/disk.img"
LOG="$W/boot.log"
QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; rm -rf "$W"; }
trap cleanup EXIT

qemu-img create -f raw "$IMG" 10G >/dev/null 2>&1

# -m 1G is deliberate — see the RAM requirement at the top. NOT -no-reboot:
# the reboot endpoint resets the machine in place, which -no-reboot turns
# into a QEMU exit.
qemu-system-x86_64 -cdrom "$ISO" \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -display none -m 1G -smp 1 -boot d -monitor none \
    -serial file:"$LOG" 2>/dev/null &
QPID=$!

wait_health() {   # $1 = label (for the failure message)
    for i in $(seq 1 90); do
        if curl -sf --max-time 2 "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            echo "FAIL  $1: QEMU exited before /api/health answered" >&2
            return 1
        fi
        sleep 2
    done
    echo "FAIL  $1: /api/health never answered within the poll window" >&2
    return 1
}

if ! wait_health "cold boot"; then
    tail -20 "$LOG" >&2
    exit 1
fi

# ─── Cold bench: everything compiled, nothing cached. ──────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/qemu/bench" \
    -H "Authorization: Bearer $TOK" -d '{"loads":500}' --max-time 120 2>&1)" || {
    echo "FAIL  cold bench request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/cold.json"

# ─── Checkpoint: TBs written to NVMe. ──────────────────────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/checkpoint" \
    -H "Authorization: Bearer $TOK" --max-time 60 2>&1)" || {
    echo "FAIL  checkpoint request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/checkpoint.json"

# ─── Reboot. The reset lands mid-response, so the curl "fails" — expected. ─
curl -s -X POST "http://127.0.0.1:$PORT/api/node/reboot" \
    -H "Authorization: Bearer $TOK" -d '{"confirm":"reboot"}' --max-time 10 \
    >/dev/null 2>&1 || true

if ! wait_health "warm boot"; then
    tail -20 "$LOG" >&2
    exit 1
fi

# ─── Warm bench: everything hit, nothing compiled. ─────────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/qemu/bench" \
    -H "Authorization: Bearer $TOK" -d '{"loads":500}' --max-time 120 2>&1)" || {
    echo "FAIL  warm bench request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/warm.json"

kill "$QPID" 2>/dev/null || true
QPID=""

if out="$(validate "$W")"; then
    echo "PASS  tcache round-trip: cold compiled, checkpoint synced, reboot restored, warm all-hit"
    exit 0
fi
echo "$out" | sed 's/^/      /'
echo "FAIL  tcache round-trip violated" >&2
exit 1

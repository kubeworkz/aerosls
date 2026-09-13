#!/usr/bin/env bash
# tests/tcache_roundtrip_check.sh — the Phase 2 persistent translation cache
# must survive a reboot, on the decoder build, every time it is measured.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The Phase 2 headline (plan §3b-3e, 2026-08-04) — blocks compiled on a cold
# boot, 0 after a checkpoint + reboot, identical guest result — was measured
# by hand on a cluster node. It predates the M8 flip, and the re-measurement
# on the decoder build (iteration 32) was ALSO by hand. A property that is
# only verified when someone remembers to measure it rots the day a change
# makes the round-trip silently not happen: the guard passes nothing and
# nobody notices, until a warm boot recompiles the world and the "compiled
# code survives reboot" claim quietly dies.
#
# This guard is the machine: boot, sweep-bench (everything compiled),
# checkpoint (TBs to NVMe), reboot, sweep-bench again (everything hit,
# nothing compiled), assert the invariant per loads value.
#
# ─── Why a sweep, not one bench ────────────────────────────────────────────
# The original guard benched ONE shape: loads=500, which translates to 8
# blocks. A round-trip that only ever proves one block count says nothing
# about the others — a regression that broke, say, single-block launches
# would sail through. The sweep benches sixteen loads values at once, one
# per block count 1..11 then 13..17 (measured; see the values above for why
# 12 is skipped):
#
#     loads:  1  65  129  193  ...  641  682  750  833  897  961
#     blocks: 1   2    3    4    ...   11   13   14   15   16   17
#
# Sixteen distinct block counts round-trip through ONE checkpoint + reboot.
# The trick that makes that possible is per-GPA placement: POST
# /api/qemu/bench_sweep (net/http.c) runs each value through
# sls_bench_load_path_at(), which places the program at its own 128 KiB-
# aligned guest GPA instead of guest physical 0. Each program then owns its
# own page — and its own tcache page digest — so the values do not
# invalidate each other (the way two programs at the same GPA would), and a
# warm sweep hits every value's blocks, not just the last one benched.
#
# ─── What it asserts, and why in invariant form ────────────────────────────
# The plan's "8/8 hits, 0 blocks compiled, identical 502 insns" is asserted
# per value, as the invariant it really is, not as magic numbers:
#
#   cold value:  ok=true, cold=true, tcache_hits=0, blocks>=1,
#                insns == loads+2 (the bench program completes)
#   checkpoint:  status=0, and the log shows "synced: N TBs"
#   boot 1 log:  "no snapshot" (NVMe up AND the disk was fresh), and NOT
#                "NVMe unavailable" (the >4 GiB BAR degradation, below)
#   boot 2 log:  "warm start" (the cache was restored from NVMe)
#   warm value:  ok=true, cold=false, blocks=0, tcache_misses=0,
#                tcache_hits == cold blocks, insns == cold insns
#
# warm_hits == cold_blocks per value is the round-trip contract: every block
# compiled for that value on the cold boot must be a hit on the warm boot,
# with nothing new compiled and the same instruction count. If TCG changes
# the block count (8 -> 9), the guard follows the kernel instead of
# false-failing.
#
# The sweep must ALSO span multiple block counts: the cold blocks must be
# strictly increasing in loads. If a TCG change merged everything into one
# block per value, every value would still round-trip — and the guard would
# be asserting the same shape five times. Strict increase pins that the
# multi-block-count claim is real.
#
# And the block counts are pinned to the measured sequence (1..11 then
# 13..17 — see the values above for the 12-block gap): a cold sweep whose
# blocks differ is a stale measurement OR a deliberate translator change.
# The former must fail loudly; the latter is blessed with
# --shape '<new sequence>', the explicit acknowledgment that the measured
# property changed.
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
# (cold_sweep.json, checkpoint.json, boot.log, warm_sweep.json) — no boot,
# no qemu, no ISO. --shape SEQ (with --replay) replaces the pinned
# block-count sequence with an explicit one, the documented way to bless a
# deliberate translator change. tests/tcache_roundtrip_smoke.sh synthesizes
# a well-formed artifact set, mutates each assertion input, and asserts the
# guard fails on each — so a regression that makes the guard blind (a wrong
# field, a dropped grep) fails the smoke instead of passing authority.
#
# Exit: 0 pass, 1 fail, 2 abort (prerequisite missing).
# GUARD-KIND: build
set -u
cd "$(dirname "$0")/.."   # repo root

ISO="${TCACHE_ISO:-sls_operating_system.iso}"
PORT="${TCACHE_PORT:-}"
TOK="deadbeef01234567cafebabe76543210"   # dave, DB_ADMIN — kernel/auth.c

# The sweep: sixteen loads values whose programs translate to sixteen
# distinct block counts. One TB holds 64 instructions, so loads 64k-63 -> k
# blocks while the program fits one 4 KiB page (k <= 11). A program larger
# than one page always lands 682 instructions on page 1, whose split wastes
# enough that the crossing starts at 13 blocks -- so 12 is genuinely
# unattainable with the bench's straight-line shape, and the sweep is
# counts 1..11 then 13..17 (loads 682..961, each at its own GPA). The JSON
# form is the request body; the space form is handed to the validator so it
# can check every requested value actually came back.
SWEEP="1 65 129 193 257 321 385 449 513 577 641 682 750 833 897 961"
SWEEP_JSON="[1,65,129,193,257,321,385,449,513,577,641,682,750,833,897,961]"

# ─── The pinned block-count sequence (iteration 37) ────────────────────────
# The measured shape for the sweep is 1..11 then 13..17 (one TB holds 64
# instructions, so loads 64k-63 -> k blocks while the program fits one page;
# a larger program always starts at 13 — the documented 12-block gap). That
# sequence is PINNED as a second canary: a cold sweep whose blocks differ
# from the pin is either a stale measurement or a deliberate translator
# change. The latter is blessed explicitly with --shape '<new sequence>';
# the former fails loudly. The 12-block gap check below only runs while the
# default pin is in force — an explicit --shape is the acknowledgment that
# the measured property changed.
PINNED_SHAPE="1 2 3 4 5 6 7 8 9 10 11 13 14 15 16 17"
SHAPE=""
SHAPE_DEFAULTED=1
REPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --replay) REPLAY="${2:-}"; shift 2 ;;
        --shape)  SHAPE="${2:-}"; SHAPE_DEFAULTED=0; shift 2 ;;
        *) echo "ABORT: unknown argument '$1' (usage: $0 [--replay DIR] [--shape '1 2 3 ... 17'])" >&2; exit 2 ;;
    esac
done
[ -z "$SHAPE" ] && SHAPE="$PINNED_SHAPE"
[ -z "$REPLAY" ] || [ -d "$REPLAY" ] || {
    echo "ABORT: --replay dir '$REPLAY' not found" >&2; exit 2
}

command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not found — the JSON assertions run in python." >&2
    exit 2
}

# ─── The validation half. Shared by the live run and --replay. ─────────────
# One python pass over all four artifacts; every assertion failure is a
# line. Missing or unparseable artifacts are failures too — a guard that
# could not read its inputs must not pass.
validate() {   # $1 = workdir
    python3 - "$1" "$SWEEP" "$SHAPE" "$SHAPE_DEFAULTED" <<'PYEOF'
import json, os, re, sys
W, sweep_csv, shape_csv, shape_defaulted = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
expected = [int(x) for x in sweep_csv.split()]
shape = [int(x) for x in shape_csv.split()]
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

cold = jf("cold_sweep.json")
chk  = jf("checkpoint.json")
warm = jf("warm_sweep.json")
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

def sweep_results(doc, name):
    if doc is None:
        return None
    if not isinstance(doc, dict) or "results" not in doc:
        fails.append("%s: missing 'results' array -- this is not a bench_sweep "
                     "response (the single-bench shape proves nothing here)" % name)
        return None
    r = doc["results"]
    if not isinstance(r, list):
        fails.append("%s: 'results' is not an array" % name)
        return None
    if len(r) != len(expected):
        fails.append("%s: %d results, expected %d -- the sweep shape changed "
                     "(did the endpoint start dropping or duplicating values?)"
                     % (name, len(r), len(expected)))
        return None
    return r

cr = sweep_results(cold, "cold_sweep.json")
wr = sweep_results(warm, "warm_sweep.json")

if cr is not None and wr is not None:
    cold_blocks = []
    for i, want in enumerate(expected):
        c, w = cr[i], wr[i]
        # ── the cold half: everything compiled, nothing cached ──────────
        if c.get("loads") != want:
            fails.append("cold value %d: loads=%r -- expected %d (the sweep "
                         "order must be preserved)" % (i, c.get("loads"), want))
        if c.get("ok") != "true":
            fails.append("cold value %d (loads=%d): ok=%r -- expected true"
                         % (i, want, c.get("ok")))
        if c.get("cold") != "true":
            fails.append("cold value %d (loads=%d): cold=%r -- expected true: "
                         "the first launch must be a fresh cold start"
                         % (i, want, c.get("cold")))
        if c.get("tcache_hits") != 0:
            fails.append("cold value %d (loads=%d): tcache_hits=%r -- expected "
                         "0: a cold boot must compile everything"
                         % (i, want, c.get("tcache_hits")))
        cb = c.get("blocks"); ci = c.get("insns")
        if not isinstance(cb, int) or cb < 1:
            fails.append("cold value %d (loads=%d): blocks=%r -- expected >= 1 "
                         "compiled" % (i, want, cb))
        if not isinstance(ci, int) or ci != want + 2:
            fails.append("cold value %d (loads=%d): insns=%r -- expected %d: "
                         "the bench program is loads+2 instructions and must "
                         "complete" % (i, want, ci, want + 2))
        cold_blocks.append(cb)
        # ── the warm half: the restored cache, nothing recompiled ───────
        if w.get("loads") != want:
            fails.append("warm value %d: loads=%r -- expected %d (the sweep "
                         "order must be preserved)" % (i, w.get("loads"), want))
        if w.get("ok") != "true":
            fails.append("warm value %d (loads=%d): ok=%r -- expected true"
                         % (i, want, w.get("ok")))
        if w.get("cold") != "false":
            fails.append("warm value %d (loads=%d): cold=%r -- expected false: "
                         "the warm run must hit the restored cache, not recompile"
                         % (i, want, w.get("cold")))
        if w.get("blocks") != 0:
            fails.append("warm value %d (loads=%d): blocks=%r -- expected 0: "
                         "nothing may be compiled on the warm run"
                         % (i, want, w.get("blocks")))
        if w.get("tcache_misses") != 0:
            fails.append("warm value %d (loads=%d): tcache_misses=%r -- "
                         "expected 0" % (i, want, w.get("tcache_misses")))
        if w.get("tcache_hits") != cb:
            fails.append("warm value %d (loads=%d): tcache_hits=%r != cold "
                         "blocks=%r -- the restored TBs were not all hit (the "
                         "round-trip contract)" % (i, want, w.get("tcache_hits"), cb))
        if w.get("insns") != ci:
            fails.append("warm value %d (loads=%d): insns=%r != cold insns=%r -- "
                         "the guest result differed across the reboot"
                         % (i, want, w.get("insns"), ci))
    # ── the sweep must actually span multiple block counts ──────────────
    if len(cold_blocks) >= 2:
        for i in range(1, len(cold_blocks)):
            if cold_blocks[i] <= cold_blocks[i - 1]:
                fails.append("cold blocks not strictly increasing across the "
                             "sweep (%s) -- the sweep must span multiple block "
                             "counts, or the multi-count claim is vacuous"
                             % cold_blocks)

    # ── the pinned block-count sequence (iteration 37) ────────────────────
    # The measured shape 1..11,13..17 is a canary on the translator's block
    # split, one level up from the round-trip invariants: a cold sweep whose
    # blocks differ from the pin means either the sweep values or the
    # iteration-35 doc are stale, or the translator changed deliberately.
    # The deliberate path is blessed with --shape '<new sequence>'; anything
    # else must fail loudly with the exact mismatch so the fix is obvious.
    if cold_blocks != shape:
        fails.append("cold sweep blocks %s != pinned shape (%s) -- the measured "
                     "block-count sequence changed; if this is a deliberate "
                     "translator change, re-run with --shape '%s' to bless the "
                     "new sequence, otherwise the sweep values and the "
                     "iteration-35 doc are stale"
                     % (cold_blocks, shape, " ".join(str(x) for x in shape)))

    # ── the documented 12-block gap (iteration 35) ────────────────────────
    # The bench program can never compile exactly 12 blocks: while it fits
    # one 4 KiB page, blocks == ceil((loads+2)/64) (1..11); any larger
    # program always lands 682 instructions on its first page and starts
    # at 13. A value reporting 12 therefore means the translator's split
    # changed -- the sweep's value set and the iteration-35 doc are stale,
    # and that must be a loud failure, not a silently-accepted new shape.
    # Runs only under the DEFAULT pin: an explicit --shape is the
    # acknowledgment that the measured property changed, so a blessed
    # sequence containing 12 must not be rejected by the stale negative.
    # (Both are canaries on measured translator properties, not
    # correctness invariants: the round-trip itself is still asserted per
    # value above.)
    if shape_defaulted == "1" and 12 in cold_blocks:
        fails.append("cold sweep contains a 12-block value (%s) -- the "
                     "documented page-crossing gap says 12 is unattainable with "
                     "the bench's straight-line shape; if the translator changed, "
                     "update the sweep values and the iteration-35 doc"
                     % cold_blocks)

if chk is not None and chk.get("status") != 0:
    fails.append("checkpoint status=%r -- expected 0 (the tcache sync to NVMe "
                 "failed)" % chk.get("status"))

print("\n".join(fails))
sys.exit(1 if fails else 0)
PYEOF
}

# ─── Replay mode: validate recorded artifacts, no boot. ────────────────────
if [ -n "$REPLAY" ]; then
    if out="$(validate "$REPLAY")"; then
        echo "PASS  replay: artifacts in $REPLAY satisfy the sweep round-trip"
        exit 0
    fi
    echo "$out" | sed 's/^/      /'
    echo "FAIL  replay: artifacts in $REPLAY violate the sweep round-trip"
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

# Port. The first free one in 3001..3020 (a live node on a deploy host, a
# previous boot) — see tests/free_port.sh for why and how it probes.
if [ -z "$PORT" ]; then
    PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free port in 3001..3020" >&2; exit 2; }
fi

W="$(mktemp -d)"
IMG="$W/disk.img"
LOG="$W/boot.log"
SER="$W/ser"
QPID=""
CATPID=""
# The serial backend must be BIDIRECTIONAL (a pipe, not a file): this
# check's guest is the classic kernel HTTP world (grub entry "kernel
# only"), which the helper selects by sending keys into the serial input
# fifo before grub's countdown auto-boots the Phase 5 initrd entry (see
# tests/grub_select_kernel_only.sh). Guest output still lands in $LOG via
# the cat, exactly like the old -serial file:.
rm -f "$SER.in" "$SER.out"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; [ -n "$CATPID" ] && kill "$CATPID" 2>/dev/null || true; rm -rf "$W"; }
trap cleanup EXIT

qemu-img create -f raw "$IMG" 10G >/dev/null 2>&1

# Accelerator: KVM when the host exposes it; otherwise an explicit,
# multi-threaded TCG fallback — the documented no-KVM mode. QEMU's own
# fallback to TCG is silent and single-threaded, so on a host without
# /dev/kvm (e.g. this project's Hetzner VPS build host: no vmx/svm, no
# nested virt) every runtime guard ran TCG anyway — choosing it explicitly
# makes the mode visible and gives SMP guests the MTTCG threads.
# QEMU_ACCEL overrides the detection entirely (e.g. QEMU_ACCEL="-accel kvm").
ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
        ACCEL="-accel kvm"
    else
        ACCEL="-accel tcg,thread=multi"
    fi
fi

# -m 1G is deliberate — see the RAM requirement at the top. NOT -no-reboot:
# the reboot endpoint resets the machine in place, which -no-reboot turns
# into a QEMU exit.
qemu-system-x86_64 -cdrom "$ISO" \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -display none -m 1G -smp 1 -boot d -monitor none \
    $ACCEL \
    -serial pipe:"$SER" 2>/dev/null &
QPID=$!

# Select grub's "kernel only" entry before the countdown auto-boots the
# Phase 5 initrd entry (which never starts the HTTP server this check
# polls — see tests/grub_select_kernel_only.sh).
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" || {
    echo "FAIL  could not select the 'kernel only' grub entry (QEMU or grub failed)" >&2
    tail -20 "$LOG" >&2
    exit 1
}

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

# ─── Cold sweep: every value compiled, nothing cached. ─────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/qemu/bench_sweep" \
    -H "Authorization: Bearer $TOK" -d "{\"loads\":$SWEEP_JSON}" --max-time 180 2>&1)" || {
    echo "FAIL  cold sweep request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/cold_sweep.json"

# ─── Checkpoint: TBs written to NVMe. ──────────────────────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/checkpoint" \
    -H "Authorization: Bearer $TOK" --max-time 60 2>&1)" || {
    echo "FAIL  checkpoint request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/checkpoint.json"

# ─── Reboot. The reset lands mid-response, so the curl "fails" — expected. ─
# The curl is BACKGROUNDED: the reset tears the connection down whenever
# the kernel processes the request, and the helper below must already be
# polling by then — a synchronous curl can block until its 10 s max-time
# (the reset never completes the response), by which point grub's warm
# menu has rendered AND its 3 s countdown has auto-booted entry 0, and the
# helper's stale-offset poll can never catch a menu it never saw.
curl -s -X POST "http://127.0.0.1:$PORT/api/node/reboot" \
    -H "Authorization: Bearer $TOK" -d '{"confirm":"reboot"}' --max-time 10 \
    >/dev/null 2>&1 &

# The reboot resets the machine in place: grub runs again and its
# countdown would auto-boot the Phase 5 entry for the warm boot too, so
# select "kernel only" once more (the helper only matches a menu render
# newer than its own start, so this cannot hit the first boot's menu).
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" || {
    echo "FAIL  could not select the 'kernel only' grub entry after reboot" >&2
    tail -20 "$LOG" >&2
    exit 1
}

if ! wait_health "warm boot"; then
    tail -20 "$LOG" >&2
    exit 1
fi

# ─── Warm sweep: every value hit, nothing compiled. ────────────────────────
body="$(curl -s -X POST "http://127.0.0.1:$PORT/api/qemu/bench_sweep" \
    -H "Authorization: Bearer $TOK" -d "{\"loads\":$SWEEP_JSON}" --max-time 180 2>&1)" || {
    echo "FAIL  warm sweep request failed: $body" >&2
    exit 1
}
printf '%s' "$body" > "$W/warm_sweep.json"

kill "$QPID" 2>/dev/null || true
QPID=""

if out="$(validate "$W")"; then
    echo "PASS  tcache round-trip: sweep cold-compiled, checkpoint synced, reboot restored, warm all-hit at $SWEEP"
    exit 0
fi
echo "$out" | sed 's/^/      /'
echo "FAIL  tcache round-trip violated" >&2
exit 1

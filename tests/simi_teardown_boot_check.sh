#!/usr/bin/env bash
# Boot the ISO under QEMU with the NIC, upload simi_recycle.bin, spawn it
# through the HTTP API with dave's token (uid 1000), and verify the
# Phase 14b (LPAR) SIMI activation-cache destroy-time story: each
# create -> assign(uid 1000) -> valloc -> upload-SIMI -> spawn -> destroy
# cycle must reclaim the SIMI activation-cache code frames its spawns
# cached, proven by (a) 18 per-cycle "[SIMI] activation vfree" lines,
# (b) the absence of "activation table is full" — the arithmetic tooth:
# the cache has 16 slots and the 18 cycles use 18 unique object names, so
# without the destroy-time free cycle 16 would fill the table — and
# (c) a system-wide frame-count stability assertion across the loop.
#
# Each cycle spawns the SIMI object TWICE: the first is an activation-cache
# MISS (translate + cache + map), the second a HIT (reuse the shared code
# frames) — so the check also proves the shared-frame path survives
# teardown and that per-process teardown still skips the cached frames
# (simi_frame_is_cached).
#
# After the 18 cycles, simi_recycle runs the Phase 14c eager-free probe:
# a HELD async spawn maps the frames (mappers=1), the object is vfree'd
# while that mapper is live (activation RETIRED, frames held — the kernel
# must log "retired, 1 live mapper(s)", NOT a free), then the held child
# is killed — its teardown walk drops the refcount to 0, which must free
# the frames IMMEDIATELY (the kernel logs "last mapper exited"), not at
# the probe partition's destroy (which must contribute no destroy-time
# activation-vfree line).
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
#
# Usage:
#   bash tests/simi_teardown_boot_check.sh   # from the repo root
set -u
cd "$(dirname "$0")/.." || exit 1

SER=/tmp/sls_serial_simi
rm -f "$SER.in" "$SER.out" boot_simi.log
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true

cat "$SER.out" > boot_simi.log &
CATPID=$!

qemu-system-x86_64 -cdrom sls_operating_system.iso \
    -drive id=disk,file=sls_storage.img,if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp::3003-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:03 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    -serial pipe:"$SER" 2>/dev/null &
QPID=$!

# Cleanup on any exit path (including SIGTERM from run_checks.sh's per-guard
# timeout): a wedged QEMU must not outlive the check. ${VAR:-} keeps the
# trap safe under `set -u` even if it fires before the vars are set. EXIT
# and TERM/INT are separate: an `exit` inside the EXIT trap would corrupt
# the guard's normal exit status, and a signal trap must terminate the
# script (bash would otherwise resume it after the trap ran).
cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# Wait for the HTTP API to come up (boot log prints the listener line).
saw_http=0
for i in $(seq 1 120); do
    if [ -f boot_simi.log ] && grep -aq "Listening on port 3000" boot_simi.log 2>/dev/null; then
        saw_http=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
[ "$saw_http" -eq 1 ] || { echo "FAILED: HTTP listener not seen"; kill "$QPID" 2>/dev/null; exit 1; }

sleep 2
python3 utils/program_upload.py --host http://localhost:3003 \
                                --file user/examples/simi_recycle.bin \
                                --name simi_recycle >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload simi_recycle" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}

metrics_frames() {
    curl -s --max-time 300 http://localhost:3003/api/metrics \
         -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
         2>/dev/null | sed -n 's/.*"ram_allocated_frames":\([0-9]*\).*/\1/p'
}
frames_before=$(metrics_frames)

curl -s --max-time 300 -X POST http://localhost:3003/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"simi_recycle"}' >/dev/null 2>&1 || {
    echo "FAILED: spawn request failed" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
sleep 25
frames_after=$(metrics_frames)

# Stop QEMU. Bounded so a wedged QEMU (SIGTERM-ignoring, stuck in D state,
# etc.) can never block the check forever: SIGTERM, 10s grace, SIGKILL.
# kill -0 succeeds on a ZOMBIE (a just-killed QEMU not yet reaped), so the
# poll also breaks on stat Z — otherwise every normal exit would burn the
# full 10s waiting on a corpse; `wait` below then reaps it instantly.
kill "$QPID" 2>/dev/null || true
for _i in $(seq 1 10); do
    kill -0 "$QPID" 2>/dev/null || break
    case "$(ps -o stat= -p "$QPID" 2>/dev/null)" in
        Z*|'') break ;;
    esac
    sleep 1
done
kill -9 "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$CATPID" 2>/dev/null || true

fail=0
grep -aq "\[sirc\] SIMI activation teardown recycle test starting" boot_simi.log || {
    echo "FAILED: simi_recycle did not start" >&2
    fail=1
}
# Every cycle must complete: create -> assign -> valloc -> upload -> spawn
# x2 -> destroy.
for c in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17; do
    grep -aq "\[sirc\] cycle $c ok" boot_simi.log || {
        echo "FAILED: cycle $c did not complete" >&2
        grep -a "sirc\]" boot_simi.log | tail -20 >&2
        fail=1
    }
done
grep -aq "\[sirc\] DONE: 18/18 cycles passed" boot_simi.log || {
    echo "FAILED: recycle loop did not complete 18/18" >&2
    grep -a "sirc\]" boot_simi.log | tail -30 >&2
    fail=1
}
grep -aq "\[sirc\] done (FAILURES)" boot_simi.log && {
    echo "FAILED: simi_recycle reported failures" >&2
    fail=1
}
# Each cycle's first spawn must have translated + cached (MISS) and its
# second must have reused the shared code frames (HIT).
miss_n=$(grep -ac "activation cache MISS — translated" boot_simi.log)
if [ "$miss_n" -lt 18 ]; then
    echo "FAILED: expected >=18 activation-cache MISS lines, saw $miss_n" >&2
    grep -a "SIMI\].*cache" boot_simi.log | head -20 >&2
    fail=1
else
    echo "ok:   $miss_n activation-cache MISS (translate + cache)"
fi
hit_n=$(grep -ac "activation cache HIT" boot_simi.log)
if [ "$hit_n" -lt 18 ]; then
    echo "FAILED: expected >=18 activation-cache HIT lines, saw $hit_n" >&2
    grep -a "SIMI\].*cache" boot_simi.log | head -20 >&2
    fail=1
else
    echo "ok:   $hit_n activation-cache HIT (shared code frames reused)"
fi
# The Phase 14b destroy-time story: every destroy must free its
# partition's activation code frames — exactly one vfree line per cycle
# (18 total; the eager-free probe's activation is freed BEFORE its
# partition's destroy, so it must contribute none).
# Count only the real frees — exclude the defensive WARNING lines that a
# broken refcount produces ("frames NOT freed"), so a sabotage can't
# inflate this count into a false pass.
simi_vfree_n=$(grep -a "\[SIMI\] activation vfree (partition .* teardown)" boot_simi.log \
               | grep -av 'WARNING' | wc -l)
if [ "$simi_vfree_n" -lt 18 ]; then
    echo "FAILED: expected >=18 SIMI activation vfree lines, saw $simi_vfree_n" >&2
    grep -a "SIMI\].*activation vfree\|sirc\]" boot_simi.log | tail -20 >&2
    fail=1
else
    echo "ok:   $simi_vfree_n SIMI activation code-frame frees at teardown"
fi
# Phase 14c eager-free probe, step 1: vfree while a mapper is LIVE must
# retire (hold the frames), not free them — the kernel logs the retired
# line with the live-mapper count.
grep -aq "\[SIMI\] activation vfree (object vfree): 'simoeager' — retired, 1 live mapper(s)" boot_simi.log || {
    echo "FAILED: vfree with a live mapper did not retire the activation (frames freed early?)" >&2
    grep -a "SIMI\].*vfree" boot_simi.log | head -10 >&2
    fail=1
}
# Phase 14c eager-free probe, step 2: killing the held child must drop the
# refcount to 0 and free the frames IMMEDIATELY — the kernel logs the
# "last mapper exited" line, and the probe's destroy must NOT log a
# destroy-time vfree for 'simoeager' (nothing left to free).
grep -aq "\[SIMI\] activation vfree (last mapper exited): 'simoeager'" boot_simi.log || {
    echo "FAILED: last mapper's exit did not free the retired activation eagerly" >&2
    grep -a "SIMI\].*vfree\|sirc\] eager" boot_simi.log | tail -10 >&2
    fail=1
}
grep -aq "\[SIMI\] activation vfree (partition .* teardown): 'simoeager'" boot_simi.log && {
    echo "FAILED: 'simoeager' frames were freed at destroy, not when the last mapper exited" >&2
    grep -a "SIMI\].*vfree" boot_simi.log | head -10 >&2
    fail=1
}
grep -aq "\[sirc\] eager-free probe DONE" boot_simi.log || {
    echo "FAILED: eager-free probe did not complete" >&2
    grep -a "sirc\] eager\|SIMI\].*vfree" boot_simi.log | tail -10 >&2
    fail=1
}
# The probe program itself must not have reported failure.
grep -aq "\[sirc\] eager-free probe FAILED" boot_simi.log && {
    echo "FAILED: eager-free probe reported failure" >&2
    fail=1
}
# The arithmetic tooth: with 18 unique object names against a 16-slot
# activation table, ANY cycle that fails to free its slot on destroy must
# log "activation table is full" — the direct signature of the leak the
# destroy-time free closes.
grep -aq "activation table is full" boot_simi.log && {
    echo "FAILED: activation table ran full — destroy-time free missing" >&2
    grep -a "SIMI\]" boot_simi.log | tail -30 >&2
    fail=1
}
# The strongest proof: the system-wide frame count must not grow across
# the create/spawn/destroy cycles (activation code frames fully reclaimed
# each time). Without the fix, 16 cycles x 1 code page leak ~16 frames —
# beyond the tolerance.
if [ -n "$frames_before" ] && [ -n "$frames_after" ]; then
    if [ "$frames_after" -gt $((frames_before + 6)) ]; then
        echo "FAILED: frame count grew across the SIMI recycle loop ($frames_before -> $frames_after)" >&2
        grep -a "SIMI\].*activation vfree\|sirc\]" boot_simi.log | tail -30 >&2
        fail=1
    else
        echo "ok:   frame count stable across SIMI recycle loop ($frames_before -> $frames_after)"
    fi
else
    echo "FAILED: could not read ram_allocated_frames from /api/metrics" >&2
    fail=1
fi
[ "$fail" -eq 0 ] || { tail -40 boot_simi.log >&2; exit 1; }

echo "OK: SIMI activation-cache teardown reclaims code frames across create/destroy cycles"
exit 0

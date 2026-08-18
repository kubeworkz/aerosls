#!/usr/bin/env bash
# Boot the ISO under QEMU with the NIC, upload part_recycle.bin, spawn it
# through the HTTP API with
# dave's token (uid 1000), and verify the Phase-2 partition-teardown wiring:
# each create -> assign(uid 1000) -> gated-spawn-into -> recv -> destroy
# cycle must reclaim the child's cap table, arena frames, page tables, and
# syscall stack, and partition_destroy must reclaim every frame the
# partition still owns — proven by a system-wide frame-count stability
# assertion across the whole loop plus the parent's final cap_list dump
# (objects active=0, arena free=16384/16384).
#
# Phase 14a (LPAR) destroy-time object story: after the 4 real cycles the
# program runs 13 spawnless probe cycles (create -> assign -> valloc ->
# upload -> destroy) that together MUST exhaust the 16-slot binary store
# unless every destroy also frees its partition's slots — probe 12 is the
# tooth (see part_recycle.c's Phase 14a comment for the arithmetic). The
# per-object half is probed too: upload 400 bytes, vfree, re-upload 96 —
# the re-upload must log total=96 (fresh slot), not total=400 (stale).
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
#
# Usage:
#   bash tests/partition_teardown_boot_check.sh   # from the repo root
set -u
cd "$(dirname "$0")/.." || exit 1

SER=/tmp/sls_serial
rm -f "$SER.in" "$SER.out" boot_part.log
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true

cat "$SER.out" > boot_part.log &
CATPID=$!

# The binaries this check uploads are gitignored build artifacts
# (user/examples/*.bin), so a fresh checkout — the server's deploy gate,
# CI, a new clone — has none, and program_upload.py fails reading them.
# Build on the spot: host gcc + objcopy, no cross toolchain (the Makefile's
# "user-programs" rules; the embedded blob headers regenerate via xxd and
# the SIMI host tools when their sources are newer). Fails fast, before
# QEMU boots.
make user-programs || {
    echo "FAILED: could not build the ring-3 programs (make user-programs)" >&2
    exit 1;
}

# The storage image this check boots is a PRIVATE FRESH one in a temp dir,
# deleted on exit — never the repo's sls_storage.img. Why:
#   1. The deploy gate (deploy/deploy.sh) runs these checks while the live
#      instance still serves. QEMU opens a raw image read-write with an
#      exclusive flock, so a second QEMU on the real image dies instantly
#      ("Failed to get \"write\" lock — Is another process using the image").
#      That is exactly the 2026-08-18 deploy-gate failure: four boot checks
#      with four zero-byte serial logs, while tcache_roundtrip_check — which
#      boots a private image — stayed green.
#   2. CI runs run_checks.sh --require-all on a fresh checkout where
#      sls_storage.img does not exist at all.
#   3. A fresh image is deterministic: this check uploads the programs it
#      asserts on and never reads persisted state, so booting empty storage
#      is the cleanest baseline (the empty-disk init path is itself proven —
#      CI's runtime smoke and tcache both boot fresh images).
# STORAGE_IMG overrides the private image with an explicit path (the
# backup/*.sh convention) for pointing a check at a real image on purpose.
W="$(mktemp -d)"
IMG="${STORAGE_IMG:-$W/disk.img}"
if [ -z "${STORAGE_IMG:-}" ]; then
    qemu-img create -f raw "$IMG" 10G >/dev/null 2>&1
fi

# Accelerator: KVM when the host exposes it; otherwise an explicit,
# multi-threaded TCG fallback — the documented no-KVM mode. QEMU's own
# fallback to TCG is silent and single-threaded, so on a host without
# /dev/kvm (e.g. this project's Hetzner VPS build host: no vmx/svm, no
# nested virt) every boot check ran TCG anyway — choosing it explicitly
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

qemu-system-x86_64 -cdrom sls_operating_system.iso \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp::3002-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:02 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>/dev/null &
QPID=$!

# Cleanup on any exit path (including SIGTERM from run_checks.sh's per-guard
# timeout): a wedged QEMU must not outlive the check. ${VAR:-} keeps the
# trap safe under `set -u` even if it fires before the vars are set. EXIT
# and TERM/INT are separate: an `exit` inside the EXIT trap would corrupt
# the guard's normal exit status, and a signal trap must terminate the
# script (bash would otherwise resume it after the trap ran).
cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# Wait for the HTTP API to come up (boot log prints the listener line).
saw_http=0
for i in $(seq 1 120); do
    if [ -f boot_part.log ] && grep -aq "Listening on port 3000" boot_part.log 2>/dev/null; then
        saw_http=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
[ "$saw_http" -eq 1 ] || { echo "FAILED: HTTP listener not seen"; kill "$QPID" 2>/dev/null; exit 1; }

sleep 2
# The child (cap_recycle_child) is NOT uploaded here: part_recycle
# creates its PROGRAM object and uploads the embedded child binary from
# ring-3 each cycle, so the object is born inside the fresh partition
# (HTTP uploads would create a partition-0 object, which the partition
# boundary in catalog_check_access() would deny at spawn time).
python3 utils/program_upload.py --host http://localhost:3002 \
                                --file user/examples/part_recycle.bin \
                                --name part_recycle >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload part_recycle" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}

metrics_frames() {
    curl -s --max-time 300 http://localhost:3002/api/metrics \
         -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
         2>/dev/null | sed -n 's/.*"ram_allocated_frames":\([0-9]*\).*/\1/p'
}
frames_before=$(metrics_frames)

curl -s --max-time 300 -X POST http://localhost:3002/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"part_recycle"}' >/dev/null 2>&1 || {
    echo "FAILED: spawn request failed" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
sleep 20
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
grep -aq "\[prec\] partition teardown recycle test starting" boot_part.log || {
    echo "FAILED: part_recycle did not start" >&2
    fail=1
}
# Every cycle must complete: create -> assign -> spawn -> recv -> destroy.
for c in 0 1 2 3; do
    grep -aq "\[prec\] cycle $c ok" boot_part.log || {
        echo "FAILED: cycle $c did not complete" >&2
        grep -a "prec\]" boot_part.log | tail -20 >&2
        fail=1
    }
done
grep -aq "\[prec\] DONE: 4/4 cycles passed" boot_part.log || {
    echo "FAILED: recycle loop did not complete 4/4" >&2
    grep -a "prec\]" boot_part.log | tail -30 >&2
    fail=1
}
grep -aq "\[prec\] done (FAILURES)" boot_part.log && {
    echo "FAILED: part_recycle reported failures" >&2
    fail=1
}
# Phase 14a (LPAR) destroy-time object story: every destroy must also
# free the binary-store slots the partition's uploads consumed. Each real
# cycle uploads one child binary, and each of the 13 probes uploads one
# more into its own fresh partition — with only 16 slots total and no
# destroy-time free, probe 12 would hit "binary store full" and the probe
# summary would not reach 13/13.
grep -aq "\[prec\] probe DONE: 13/13 uploads ok" boot_part.log || {
    echo "FAILED: binary-store probe loop did not reach 13/13 (slots leaked?)" >&2
    grep -a "prec\] probe\|LOADER\] upload: binary store full\|prec\]" boot_part.log | tail -30 >&2
    fail=1
}
grep -aq "\[LOADER\] upload: binary store full" boot_part.log && {
    echo "FAILED: binary store ran full during the recycle loop" >&2
    fail=1
}
# The loader-side teardown must have run: at least one slot freed per
# real cycle (cr_c0..cr_c3) plus one per probe partition.
loader_vfree_n=$(grep -ac "\[LOADER\] vfree (partition .* teardown): 'cr_c" boot_part.log)
if [ "$loader_vfree_n" -lt 4 ]; then
    echo "FAILED: expected >=4 loader slot frees for the real cycles, saw $loader_vfree_n" >&2
    grep -a "LOADER\] vfree\|LOADER\] upload: binary store full" boot_part.log | head -20 >&2
    fail=1
else
    echo "ok:   $loader_vfree_n child binary-store slots freed at teardown"
fi
# Per-object half: sys_sls_vfree must free the object's binary-store slot.
# The vfree probe uploads 400 bytes, vfrees, then re-uploads 96 bytes of
# the same name — the re-upload must log total=96 (a fresh slot). Without
# the slot free, loader_get_or_alloc() reuses the stale active slot and
# only grows size, so the kernel would log total=400 instead: the 96-byte
# line is the tooth.
grep -aq "\[prec\] vfree probe DONE" boot_part.log || {
    echo "FAILED: vfree probe did not complete" >&2
    grep -a "prec\] vfree probe\|vfreeprobe" boot_part.log | tail -20 >&2
    fail=1
}
grep -aq "\[prec\] vfree probe FAILED" boot_part.log && {
    echo "FAILED: vfree probe reported failure" >&2
    fail=1
}
grep -aq "\[LOADER\] 'vfreeprobe': wrote 96 bytes at offset 0 (total=96, flat)" boot_part.log || {
    echo "FAILED: vfree probe re-upload logged total=96 — stale slot state inherited" >&2
    grep -a "vfreeprobe" boot_part.log | tail -10 >&2
    fail=1
}
grep -aq "\[LOADER\] vfree (object vfree): 'vfreeprobe'" boot_part.log || {
    echo "FAILED: sys_sls_vfree did not free the binary-store slot" >&2
    grep -a "vfreeprobe\|CATALOG\] vfree" boot_part.log | tail -10 >&2
    fail=1
}
echo "ok:   sys_sls_vfree frees the binary-store slot (vfree probe)"
# Each child exits WITHOUT revoking its kept MEM cap — only teardown
# (child exit or partition_destroy) can return its arena frame. The final
# cap_list dump must show a fully returned arena and zero live objects.
grep -aq "\[CAP\] objects active=0/1024  arena free=16384/16384 frames" boot_part.log || {
    echo "FAILED: arena / objects not fully reclaimed after 4 cycles (leak)" >&2
    grep -a "CAP\] objects\|TORE\]\|prec\]" boot_part.log | tail -30 >&2
    fail=1
}
# Every child exit (or partition kill) ran the Phase-2 cap-table teardown.
tore_n=$(grep -ac "\[TORE\] PID .* teardown:" boot_part.log)
if [ "$tore_n" -lt 4 ]; then
    echo "FAILED: expected >=4 cap-table teardown lines, saw $tore_n" >&2
    grep -a "TORE\]" boot_part.log | head -20 >&2
    fail=1
else
    echo "ok:   $tore_n cap-table teardowns ran"
fi
# The strongest proof: the system-wide frame count must not grow across the
# create/spawn/destroy cycles (partition frames fully reclaimed each time).
if [ -n "$frames_before" ] && [ -n "$frames_after" ]; then
    if [ "$frames_after" -gt $((frames_before + 16)) ]; then
        echo "FAILED: frame count grew across the partition recycle loop ($frames_before -> $frames_after)" >&2
        grep -a "TORE\]\|PART\]\|prec\]" boot_part.log | tail -30 >&2
        fail=1
    else
        echo "ok:   frame count stable across partition recycle loop ($frames_before -> $frames_after)"
    fi
else
    echo "FAILED: could not read ram_allocated_frames from /api/metrics" >&2
    fail=1
fi
[ "$fail" -eq 0 ] || { tail -40 boot_part.log >&2; exit 1; }

echo "OK: partition teardown reclaims everything across create/destroy cycles"
exit 0

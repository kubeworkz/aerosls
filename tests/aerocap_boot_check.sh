#!/usr/bin/env bash
# Boot the ISO under QEMU with the NIC, upload user/examples/cap_roundtrip.bin
# through the kernel's HTTP API (hostfwd localhost:3011 -> guest :3000), spawn
# the ring-3 program, and verify the libaerocap SDK lifecycle from its serial
# output. Phases 2-3 add the two-party handoff and the live-overlap
# (Phase 1.5) flows; Phase 4 verifies Phase-2 teardown with 20 repeated
# spawn/exit cycles and a system-wide frame-count stability assertion.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
#
# Usage:
#   bash tests/aerocap_boot_check.sh     # from the repo root, after make x86-iso + make user-programs
set -u
cd "$(dirname "$0")/.." || exit 1

SER=/tmp/sls_serial
rm -f "$SER.in" "$SER.out" boot_aerocap.log
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true

cat "$SER.out" > boot_aerocap.log &
CATPID=$!

# The binaries this check uploads are gitignored build artifacts
# (user/examples/*.bin), so a fresh checkout — the server's deploy gate,
# CI, a new clone — has none, and program_upload.py fails reading them.
# Build on the spot: host gcc + objcopy, no cross toolchain (the Makefile's
# "user-programs" rules). The embedded blob headers are TRACKED generated
# artifacts whose content is already byte-identical to what the build embeds
# (the *_blob_guard checks prove it), so `make -o` keeps them from being
# regenerated: a regen would flip their mtimes to after the kernel build
# and trip stack_frame_budget_check's deliberately strict binary-newer-
# than-sources rule (a false abort — the 2026-08-18 deploy hit exactly
# that). Fails fast, before QEMU boots.
make -o user/examples/cap_recycle_child_blob.h \
     -o user/examples/simi_recycle_tmo_blob.h \
     user-programs || {
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
    -netdev user,id=net0,hostfwd=tcp::3011-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
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

# Select grub's "kernel only" entry (menu entry 1) before the 3 s
# countdown auto-boots the Phase 5 self-hosted entry: the initrd boot
# hands the machine to the init sidecar and never starts the kernel HTTP
# server this check asserts (see tests/grub_select_kernel_only.sh).
bash tests/grub_select_kernel_only.sh "$SER.in" boot_aerocap.log "$QPID" || {
    echo "FAILED: could not select the 'kernel only' grub entry (QEMU or grub failed)" >&2
    kill "$QPID" 2>/dev/null
    exit 1
}

# Wait for the HTTP API to come up (boot log prints the listener line).
saw_http=0
for i in $(seq 1 120); do
    if [ -f boot_aerocap.log ] && grep -aq "Listening on port 3000" boot_aerocap.log 2>/dev/null; then
        saw_http=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
[ "$saw_http" -eq 1 ] || { echo "FAILED: HTTP listener not seen"; kill "$QPID" 2>/dev/null; exit 1; }

# Give the e1000/DHCP a moment to settle, then upload the ring-3 example.
sleep 2
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_roundtrip.bin \
                                --name cap_roundtrip >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_roundtrip" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}

# Spawn it via the HTTP API.
curl -s --max-time 300 -X POST http://localhost:3011/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"cap_roundtrip"}' >/dev/null 2>&1 || {
    echo "FAILED: spawn request failed" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}

# Let the ring-3 program run to completion.
sleep 8

# ── Phase 2: two-party handoff (cap_two_party spawns cap_peer) ────────────
# Upload both binaries, then spawn the parent; the parent spawns the child
# itself via SYS_SLS_PROGRAM_SPAWN (synchronous: child runs to completion,
# parent resumes). Same boot, same QEMU.
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_peer.bin \
                                --name cap_peer >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_peer" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_two_party.bin \
                                --name cap_two_party >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_two_party" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
curl -s --max-time 300 -X POST http://localhost:3011/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"cap_two_party"}' >/dev/null 2>&1 || {
    echo "FAILED: two-party spawn request failed" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
sleep 8

# ── Phase 3: Phase 1.5 live overlap (non-blocking spawn + blocking recv) ─
# cap_overlap spawns cap_peer_ovl via SYS_SLS_PROGRAM_SPAWN_NB (returns
# immediately), then BLOCKS in cap_recv — the kernel parks it mid-syscall and
# iretq's into the child. The child sends the MEM cap (waking the parent),
# spins (proving both are live), sends a 'done' marker, and exits; the parent
# resumes inside its recv, reads 'Hello' back, re-parks for the marker, and
# tears down.
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_peer_ovl.bin \
                                --name cap_peer_ovl >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_peer_ovl" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_overlap.bin \
                                --name cap_overlap >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_overlap" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
curl -s --max-time 300 -X POST http://localhost:3011/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"cap_overlap"}' >/dev/null 2>&1 || {
    echo "FAILED: overlap spawn request failed" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
sleep 8

# ── Phase 4: Phase 2 teardown (repeated spawn/exit cycles, no leaks) ────
# cap_recycle spawns cap_recycle_child 20 times via the GATED non-blocking
# spawn; each child allocates MEM caps, sends one, and EXITS WITHOUT
# revoking anything — so the kernel's Phase-2 teardown must reclaim the
# child's cap table, arena frames, page tables, and syscall stack on every
# exit. 20 cycles exceeds the 15 process cap tables, so a leak of table
# bindings fails the loop itself; the /api/metrics frame count must not
# grow across the whole loop.
metrics_frames() {
    curl -s --max-time 300 http://localhost:3011/api/metrics \
         -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
         2>/dev/null | sed -n 's/.*"ram_allocated_frames":\([0-9]*\).*/\1/p'
}
frames_before=$(metrics_frames)

python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_recycle_child.bin \
                                --name cap_recycle_child >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_recycle_child" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
python3 utils/program_upload.py --host http://localhost:3011 \
                                --file user/examples/cap_recycle.bin \
                                --name cap_recycle >/dev/null 2>&1 || {
    echo "FAILED: program_upload.py could not upload cap_recycle" >&2
    kill "$QPID" 2>/dev/null; exit 1;
}
curl -s --max-time 300 -X POST http://localhost:3011/api/program/spawn \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
     -d '{"name":"cap_recycle"}' >/dev/null 2>&1 || {
    echo "FAILED: recycle spawn request failed" >&2
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
grep -aq "\[cap\] Ring-3 capability lifecycle demo" boot_aerocap.log || {
    echo "FAILED: ring-3 program did not start (no [cap] banner)" >&2
    fail=1
}
grep -aq "\[cap\] map ok (user PTE installed)" boot_aerocap.log || {
    echo "FAILED: cap_map did not install a user PTE" >&2
    grep -a "cap\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[cap\] write+read 'Hello' PASS (ring-3 user mapping)" boot_aerocap.log || {
    echo "FAILED: write+read 'Hello' did not pass through the user mapping" >&2
    grep -a "cap\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[cap\] send ok (MEM cap moved to the channel queue)" boot_aerocap.log || {
    echo "FAILED: cap_send did not move the MEM cap" >&2
    grep -a "cap\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[cap\] revoke ok (MEM object destroyed, arena frame returned)" boot_aerocap.log || {
    echo "FAILED: cap_revoke did not destroy the MEM object" >&2
    grep -a "cap\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[cap\] done" boot_aerocap.log || {
    echo "FAILED: ring-3 program did not finish" >&2
    grep -a "cap\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[cap\] done (FAILURES)" boot_aerocap.log && {
    echo "FAILED: ring-3 program reported failures" >&2
    fail=1
}

# ── Two-party assertions ────────────────────────────────────────────────────
grep -aq "\[peer\] ring-3 capability peer starting" boot_aerocap.log || {
    echo "FAILED: child (cap_peer) did not start" >&2
    fail=1
}
grep -aq "\[peer\] getppid -> " boot_aerocap.log || {
    echo "FAILED: child could not resolve its parent's pid (SYS_SLS_GETPPID)" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[peer\] chan_create ok (far_pid=parent)" boot_aerocap.log || {
    echo "FAILED: child did not mint the channel with far_pid=parent" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[peer\] send ok" boot_aerocap.log || {
    echo "FAILED: child did not send the MEM cap to the parent" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[roundtrip\] recv ok — MEM cap arrived in our table" boot_aerocap.log || {
    echo "FAILED: parent did not recv the transferred MEM cap" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[roundtrip\] mapped + read 'Hello' back from the child: PASS" boot_aerocap.log || {
    echo "FAILED: parent did not read 'Hello' back through its own mapping" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[roundtrip\] revoke ok" boot_aerocap.log || {
    echo "FAILED: parent teardown did not complete" >&2
    grep -a "peer\]\|roundtrip\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[roundtrip\] done (FAILURES)" boot_aerocap.log && {
    echo "FAILED: two-party program reported failures" >&2
    fail=1
}

# ── Phase-1.5 overlap assertions ────────────────────────────────────────────
grep -aq "\[ovl\] live-overlap test starting" boot_aerocap.log || {
    echo "FAILED: overlap parent did not start" >&2
    fail=1
}
grep -aq "\[ovl\] spawn_nb returned immediately" boot_aerocap.log || {
    echo "FAILED: non-blocking spawn did not return immediately" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[CAP\] recv block: parked PID" boot_aerocap.log || {
    echo "FAILED: kernel did not park the parent in blocking cap_recv" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[CAP\] woken PID" boot_aerocap.log || {
    echo "FAILED: child's send did not wake the parked parent" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[CAP\] send handoff: PID" boot_aerocap.log || {
    echo "FAILED: send did not hand the CPU to the woken parent immediately" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[peer2\] resumed after send — parent recv'd the MEM cap first" boot_aerocap.log || {
    echo "FAILED: async child did not run + send the MEM cap" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[peer2\] spin i=" boot_aerocap.log || {
    echo "FAILED: async child did not stay live after sending" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[ovl\] recv#1 ok — woken by child" boot_aerocap.log || {
    echo "FAILED: parent did not resume inside its recv" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
# ── Immediate-wake ORDER proof: the receiver ran BEFORE the sender's send
# syscall returned to ring-3 — "recv#1 ok" must appear EARLIER in the log
# than the child's "resumed after send" (no tick latency between them).
#
# Compared by BYTE OFFSET, not line number: under -smp 4 the serial writer
# can be preempted mid-line, so the child's print occasionally lands on the
# SAME log line as the parent's — but the parent (handoff recipient) always
# starts printing first, so its text still precedes the child's within that
# line. Line numbers tie in that case (a pre-existing flake); offsets don't.
recv1_off=$(grep -abo '\[ovl\] recv#1 ok' boot_aerocap.log | head -1 | cut -d: -f1)
child_off=$(grep -abo '\[peer2\] resumed after send' boot_aerocap.log | head -1 | cut -d: -f1)
if [ -z "$recv1_off" ] || [ -z "$child_off" ] || [ "$recv1_off" -gt "$child_off" ]; then
    echo "FAILED: immediate wake NOT proven — recv#1 (offset $recv1_off) must precede the child's send return (offset $child_off)" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -30 >&2
    fail=1
else
    echo "ok:   immediate wake — recv#1 (offset $recv1_off) ran before the sender's send returned (offset $child_off)"
fi
grep -aq "\[ovl\] mapped + read 'Hello' back from the live child: PASS" boot_aerocap.log || {
    echo "FAILED: parent did not read 'Hello' back from the live child" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[ovl\] recv#2 ok" boot_aerocap.log || {
    echo "FAILED: second blocking recv (done marker) did not complete" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[peer2\] resumed after 'done' send; exiting" boot_aerocap.log || {
    echo "FAILED: child did not resume after its 'done' send (yield handback)" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[PROC\] PID .* yielded to PID" boot_aerocap.log || {
    echo "FAILED: parent did not yield the CPU back to the child (SYS_SLS_YIELD)" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]\|PROC\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[ovl\] resumed from yield (child exited)" boot_aerocap.log || {
    echo "FAILED: parent did not resume from its yield" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[ovl\] done" boot_aerocap.log || {
    echo "FAILED: overlap parent did not finish" >&2
    grep -a "ovl\]\|peer2\]\|CAP\]" boot_aerocap.log | head -20 >&2
    fail=1
}
grep -aq "\[ovl\] done (FAILURES)" boot_aerocap.log && {
    echo "FAILED: overlap program reported failures" >&2
    fail=1
}

# ── Phase-2 teardown (recycle) assertions ──────────────────────────────────
grep -aq "\[recycle\] teardown recycle test starting" boot_aerocap.log || {
    echo "FAILED: recycle parent did not start" >&2
    fail=1
}
grep -aq "\[recycle\] DONE: 20/20 cycles passed" boot_aerocap.log || {
    echo "FAILED: recycle loop did not complete 20/20 (cap-table / resource leak would fail mid-loop)" >&2
    grep -a "recycle\]\|rc2\]\|TORE\]\|CAP\]" boot_aerocap.log | head -30 >&2
    fail=1
}
grep -aq "\[recycle\] done (FAILURES)" boot_aerocap.log && {
    echo "FAILED: recycle program reported failures" >&2
    fail=1
}
# Teardown actually ran on every child exit: one [TORE] PID teardown line per child.
tore_n=$(grep -ac "\[TORE\] PID .* teardown:" boot_aerocap.log)
if [ "$tore_n" -lt 20 ]; then
    echo "FAILED: expected >=20 cap-table teardown lines (one per child exit), saw $tore_n" >&2
    grep -a "TORE\]" boot_aerocap.log | head -30 >&2
    fail=1
else
    echo "ok:   $tore_n cap-table teardowns ran (one per child exit)"
fi
# The arena must be FULLY returned after 20 cycles (each child kept a MEM cap
# that only teardown could free) — cap_list is called by the parent just
# before its DONE line.
grep -aq "\[CAP\] objects active=0/1024  arena free=16384/16384 frames" boot_aerocap.log || {
    echo "FAILED: arena / objects not fully reclaimed after 20 cycles (leak)" >&2
    grep -a "CAP\] objects\|TORE\]" boot_aerocap.log | head -30 >&2
    fail=1
}
# The strongest leak proof: the system-wide frame count must not grow across
# the 20 spawn/exit cycles (page tables, syscall stacks, binaries all freed).
if [ -n "$frames_before" ] && [ -n "$frames_after" ]; then
    if [ "$frames_after" -gt $((frames_before + 16)) ]; then
        echo "FAILED: frame count grew across the recycle loop ($frames_before -> $frames_after)" >&2
        grep -a "TORE\]\|PROC\]" boot_aerocap.log | head -30 >&2
        fail=1
    else
        echo "ok:   frame count stable across recycle loop ($frames_before -> $frames_after)"
    fi
else
    echo "FAILED: could not read ram_allocated_frames from /api/metrics" >&2
    fail=1
fi
[ "$fail" -eq 0 ] || { tail -40 boot_aerocap.log >&2; exit 1; }

echo "OK: libaerocap ring-3 lifecycle + two-party handoff + live overlap passed"
exit 0

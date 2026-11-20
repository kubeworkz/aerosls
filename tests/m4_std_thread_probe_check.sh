#!/usr/bin/env bash
# m4_std_thread_probe_check.sh — M4.9's real std::thread two-boot gate.
#
# The gate profile's firstuser is a static musl Rust candidate using
# std::thread::spawn. That reaches musl pthread_create, whose stack is
# mmap(PROT_NONE) then mprotect(PROT_READ|PROT_WRITE): the PROT_NONE
# reservation must be backed on demand. Boot 1 checkpoints both parked
# contexts; boot 2 restores them, wakes the worker, and joins it.
#
# Build:
#   make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_STD_THREAD_PROBE=1 \
#       SIDECAR_E3_CPIO=sidecars_m4_std_thread_e3.cpio \
#       X86_E3_ISO=sls_m4_std_thread.iso
#
# Before the build, a static clause reads e7_firstuser.rs: the aerobackup
# FIRSTUSER must sit behind `not(any(... feature = "m4_std_thread_probe" ...))`
# and the empty fallback behind the matching `any(...)`. 87bf5e9 added the
# feature without those two cfg branches, so the probe image embedded BOTH
# the 580 KiB std_thread_gate fixture and the 600 KiB aerobackup payload and
# pushed the sidecar image into its upper pages (fixed in 76bd182). The clause
# fails here, before an ISO exists, if the fixture selection and the probe
# profile drift apart again.
#
# The same clause reads PROBE_RESERVE out of m4_std_thread_probe.rs and fails
# when the gate fixture no longer fits that tail reserve -- spawn answers an
# oversized fixture with Err(-1), a create failure with no image symptom.
# After the build the flattened posix.bin is asserted too: it must carry the
# gate fixture and NO aerobackup bytes, so the source-level cfg and the built
# artifact are both checked rather than observed.
#
# Usage:
#   bash tests/m4_std_thread_probe_check.sh
#   M4_STD_THREAD_BUILD=0 M4_STD_THREAD_ISO=... bash ...
#   M4_STD_THREAD_STATIC_ONLY=1 bash ...   # source assertion only, no QEMU
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M4_STD_THREAD_ISO:-sls_m4_std_thread.iso}"
CPIO="${M4_STD_THREAD_CPIO:-sidecars_m4_std_thread_e3.cpio}"
BUILD="${M4_STD_THREAD_BUILD:-1}"
ART="${M4_STD_THREAD_ARTIFACTS:-/tmp/m4-std-thread-$$}"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M4_STD_THREAD_ENTRY:-3}"

mkdir -p "$ART" || { echo "ABORT: cannot create $ART" >&2; exit 2; }

QPID=""
CATPID=""
PORT=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${CATPID:-}" ] && kill "$CATPID" 2>/dev/null
    QPID=""; CATPID=""
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }

# ── static: the probe profile must not embed the backup fixture ───────────
# The image's FIRSTUSER is selected by two cfg branches in e7_firstuser.rs.
# If m4_std_thread_probe is not on BOTH the `not(any(...))` that guards the
# aerobackup include and the `any(...)` that supplies the empty fallback, an
# M4_STD_THREAD_PROBE=1 build either embeds the backup payload alongside the
# std::thread fixture (the 87bf5e9 omission) or loses FIRSTUSER entirely.
# This is the source-level assertion of 76bd182's fix, and it also requires
# the two branches to name the same feature set, so a future probe added to
# only one branch is named here rather than at boot.
FIRSTUSER_SRC=user/sidecar/src/e7_firstuser.rs
[ -f "$FIRSTUSER_SRC" ] || fail "$FIRSTUSER_SRC is missing"
static_err=$(
    awk '
        function feats(s,    out, m) {
            out = ""
            while (match(s, /feature = "[^"]+"/)) {
                m = substr(s, RSTART + 11, RLENGTH - 12)
                out = out (out == "" ? "" : " ") m
                s = substr(s, RSTART + RLENGTH)
            }
            return out
        }
        function seteq(a, b,    na, nb, x, y, i, j, found) {
            na = split(a, x, " ")
            nb = split(b, y, " ")
            if (na != nb) return 0
            for (i = 1; i <= na; i++) {
                found = 0
                for (j = 1; j <= nb; j++) if (x[i] == y[j]) found = 1
                if (!found) return 0
            }
            return 1
        }
        {
            line = $0
            if (incfg) {
                cfg = cfg " " line
                if (line ~ /\]/) incfg = 0
                next
            }
            if (line ~ /^[[:space:]]*$/) next
            if (line ~ /^#\[cfg\(/) {
                cfg = line
                if (line !~ /\]/) incfg = 1
                next
            }
            if (line ~ /static FIRSTUSER: &\[u8\]/) {
                n++
                attr[n] = cfg
                if (line ~ /aerobackup\.musl\.elf/) kind[n] = "backup"
                else if (line ~ /= &\[\];/) kind[n] = "empty"
                else kind[n] = "unknown"
            }
            cfg = ""
        }
        END {
            for (i = 1; i <= n; i++) {
                if (kind[i] == "backup") { nb++; ba = attr[i] }
                else if (kind[i] == "empty") { ne++; ea = attr[i] }
                else nu++
            }
            if (nb != 1 || ne != 1 || nu != 0) {
                printf "%s must have exactly one aerobackup FIRSTUSER and one empty fallback (found %d backup, %d empty, %d other)\n", FILENAME, nb, ne, nu
                exit 1
            }
            if (index(ba, "#[cfg(not(any(") != 1 || \
                index(ba, "feature = \"m4_std_thread_probe\"") == 0) {
                printf "the aerobackup FIRSTUSER is not gated on not(any(... feature = \"m4_std_thread_probe\" ...)): %s\n", ba
                exit 1
            }
            if (index(ea, "#[cfg(any(") != 1 || index(ea, "not(") != 0 || \
                index(ea, "feature = \"m4_std_thread_probe\"") == 0) {
                printf "the empty FIRSTUSER fallback is not the complementary any(... feature = \"m4_std_thread_probe\" ...): %s\n", ea
                exit 1
            }
            if (!seteq(feats(ba), feats(ea))) {
                printf "FIRSTUSER cfg branches do not list the same probe features (backup: %s; empty: %s) — adding one to a single branch is how the backup fixture slips into a probe image\n", feats(ba), feats(ea)
                exit 1
            }
            exit 0
        }
    ' "$FIRSTUSER_SRC"
) || fail "${static_err:-the FIRSTUSER cfg check did not run}"
echo "static: $FIRSTUSER_SRC excludes the backup fixture under m4_std_thread_probe"

# ── static: the gate fixture must fit the tail reserve it is planted in ───
# m4_std_thread_probe::spawn plants the embedded fixture at the last
# PROBE_RESERVE bytes of the POSIX sidecar's budget heap and returns Err(-1)
# when PROBE.len() exceeds it. That refusal is a silent create failure at boot,
# so the budget is asserted here against the source's own constant -- reading
# it rather than repeating 1 MiB keeps this check and the spawn path from
# drifting apart.
PROBE_SRC=user/sidecar/src/m4_std_thread_probe.rs
GATE_FIXTURE=user/sidecar/fixtures/std_thread_gate.elf
[ -f "$PROBE_SRC" ] || fail "$PROBE_SRC is missing"
[ -f "$GATE_FIXTURE" ] || fail "$GATE_FIXTURE is missing"
reserve_expr=$(sed -n 's/^[[:space:]]*pub const PROBE_RESERVE: u64 = \(.*\);[[:space:]]*$/\1/p' "$PROBE_SRC" | head -1)
if [ -z "$reserve_expr" ] || ! printf '%s\n' "$reserve_expr" | grep -Eq '^[0-9 ()+*]+$'; then
    fail "cannot read PROBE_RESERVE from $PROBE_SRC (got '${reserve_expr}')"
fi
RESERVE=$(( reserve_expr ))
FIXTURE_BYTES=$(wc -c < "$GATE_FIXTURE" | tr -d ' \r\n')
[ "$FIXTURE_BYTES" -le "$RESERVE" ] || \
    fail "std_thread_gate.elf is $FIXTURE_BYTES bytes but the probe's tail reserve (PROBE_RESERVE) is $RESERVE — m4_std_thread_probe::spawn would refuse the fixture at boot"
echo "static: std_thread_gate.elf $FIXTURE_BYTES bytes fits the $RESERVE-byte tail reserve"

# The source assertion alone, for a host that cannot build and boot the
# profile (or a source-only CI job). It is the same clause the build arm runs
# first; only the image and the two boots are skipped.
if [ "${M4_STD_THREAD_STATIC_ONLY:-0}" = "1" ]; then
    echo "PASS: m4_std_thread_probe's FIRSTUSER cfg excludes the backup fixture"
    exit 0
fi

if [ "$BUILD" = "1" ]; then
    echo "=== build: x86-iso-e3 M4_STD_THREAD_PROBE=1 ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_STD_THREAD_PROBE=1 \
        SIDECAR_E3_CPIO="$CPIO" X86_E3_ISO="$ISO" || \
        fail "the std::thread probe image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M4_STD_THREAD_BUILD=0 needs a prebuilt one)"

grep -qF 'std_thread_gate.elf' user/sidecar/src/m4_std_thread_probe.rs || \
    fail "the probe does not embed the std::thread fixture"
grep -qF 'm4_std_thread_probe' user/sidecar/src/lib.rs || \
    fail "the std::thread probe module is not compiled"
grep -qF 'SIDECAR_M4_STD_THREAD_PROBE' Makefile || \
    fail "the Makefile has no std::thread probe feature"

# ── image: the built POSIX sidecar carries the probe fixture only ─────────
# The cfg clause above is about the source; this one is about the artifact.
# include_bytes! embeds each fixture whole, so a contiguous whole-file match is
# the exact test. The gate fixture must be there, which is the positive control
# (a posix.bin this probe build did not produce fails instead of passing
# vacuously), and the backup payload must not be: finding it means the build
# embedded BOTH the 580 KiB gate fixture and the 600 KiB aerobackup payload,
# the pre-76bd182 image that overflowed the sidecar's upper pages.
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 is required to read the built POSIX image's payload bytes" >&2
    exit 2
}
POSIX_BIN="${M4_STD_THREAD_POSIX_BIN:-$(sed -n 's/^SIDECAR_POSIX_BIN[[:space:]]*?=[[:space:]]*//p' Makefile | head -1)}"
[ -n "$POSIX_BIN" ] || POSIX_BIN=user/target/x86_64-unknown-none/release/posix.bin
[ -s "$POSIX_BIN" ] || \
    fail "no built POSIX sidecar at $POSIX_BIN — the payload assertion needs the build output (keep it with M4_STD_THREAD_BUILD=0, or set M4_STD_THREAD_POSIX_BIN)"
BACKUP_FIXTURE=user/sidecar/fixtures/aerobackup.musl.elf
[ -f "$BACKUP_FIXTURE" ] || fail "$BACKUP_FIXTURE is missing"
payload_err=$(python3 - "$POSIX_BIN" "$GATE_FIXTURE" "$BACKUP_FIXTURE" 2>&1 <<'PY'
import sys
posix, gate, backup = (open(p, "rb").read() for p in sys.argv[1:4])
if posix.find(gate) < 0:
    sys.exit(f"{sys.argv[1]} does not contain {sys.argv[2]} — the probe build did not reach this artifact")
at = posix.find(backup)
if at >= 0:
    sys.exit(f"{sys.argv[1]} contains {sys.argv[3]} at offset {at} ({len(backup)} bytes) — this build embedded the backup fixture alongside the probe's")
PY
) || fail "${payload_err:-the payload check did not run}"
echo "image: $POSIX_BIN carries std_thread_gate.elf and no aerobackup bytes"

rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"

boot() {
    local log="$1" tag="$2" ser="$ART/ser_$2"
    PORT=$(bash tests/free_port.sh) || exit 2
    rm -f "$ser.in" "$ser.out"
    mkfifo "$ser.in" "$ser.out" || fail "could not make the serial pipes"
    cat "$ser.out" > "$log" &
    CATPID=$!
    qemu-system-x86_64 -cdrom "$ROOT/$ISO" \
        -drive id=disk,file="$IMG",if=none,format=raw \
        -device nvme,drive=disk,serial=slsdev0 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
        -display none -m 1G -smp 2 -boot d -monitor none \
        -accel tcg,thread=multi \
        ${M4_STD_THREAD_QEMU_EXTRA:-} \
        -serial pipe:"$ser" 2> "$ART/${tag}_qemu.err" &
    QPID=$!
    bash tests/grub_select_kernel_only.sh "$ser.in" "$log" "$QPID" "$ENTRY" || \
        fail "grub entry $ENTRY did not boot ($tag)"
    local ready=0
    for _ in $(seq 1 480); do
        if grep -aq "unified=1" "$log" 2>/dev/null && \
           grep -aq "Listening on port 3000" "$log" 2>/dev/null && \
           [ "$(grep -acF '[INIT] heartbeat' "$log")" -ge 5 ]; then
            ready=1; break
        fi
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the control plane was up ($tag)"
        sleep 0.5
    done
    [ "$ready" -eq 1 ] || fail "the unified boot did not reach its markers ($tag)"
    echo "up($tag): control plane on $PORT"
}

api() {
    local out="$1"; shift
    local code
    code=$(curl -s --max-time 600 -w '%{http_code}' \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null)
    echo "  [api] $* -> HTTP $code (body: $(cat "$out" 2>/dev/null))" >&2
    [ "$code" = "200" ]
}
jval() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get(sys.argv[2], "")
print("" if v is None else v)
PY
}

echo "=== boot 1: spawn std::thread and park both waiters ==="
: > "$L1"
boot "$L1" one
BASE="http://127.0.0.1:$PORT"

api "$ART/pcreate.json" -X POST -d '{"name":"m4std"}' "$BASE/api/partitions" || \
    fail "POST /api/partitions"
PID=$(jval "$ART/pcreate.json" partition_id)
[ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
echo "partition=$PID"
api "$ART/create.json" -X POST -d '{"index":1}' "$BASE/api/partition/$PID/env" || \
    fail "POST env"
echo "create: $(cat "$ART/create.json")"

spawned=0
for _ in $(seq 1 240); do
    if grep -aqF "[E7] firstuser: spawned" "$L1"; then spawned=1; break; fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the std::thread probe spawned"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the std::thread probe never spawned"

parked=0
for _ in $(seq 1 240); do
    if grep -aqF "STD-PARENT-WAIT" "$L1" && \
       grep -aqF "STD-WORKER-WAIT" "$L1" && \
       [ "$(grep -acF 'wait (futex)' "$L1")" -ge 2 ]; then
        parked=1; break
    fi
    grep -aqF "STD-THREAD-FAIL" "$L1" && fail "the std::thread probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before both waiters parked"
    sleep 0.5
done
[ "$parked" -eq 1 ] || fail "the std::thread probe never parked both futex waiters"
for marker in STD-THREAD-START STD-WORKER-TID STD-WORKER-FD-OK \
              STD-WORKER-READY STD-WORKER-WAIT STD-PARENT-WAIT; do
    grep -aqF "$marker" "$L1" || fail "missing boot-1 std::thread marker: $marker"
done
grep -aF "wait (futex)" "$L1" | tail -2

# The two Linux waiters are parked; let the system sidecars reach their own
# multi-channel recv before the quiesce samples them. M4.7's smaller capture
# happened to catch them there; the std::thread stack's extra mmap/mprotect
# traffic keeps drv.ramdisk.1 awake a little longer.
sleep 3

api "$ART/checkpoint.json" -X POST "$BASE/api/checkpoint" || \
    fail "POST /api/checkpoint"
echo "checkpoint: $(cat "$ART/checkpoint.json")"
STATUS=$(jval "$ART/checkpoint.json" status)
COUNT=$(jval "$ART/checkpoint.json" count)
[ "$STATUS" = "0" ] || fail "checkpoint status=$STATUS (want 0)"
case "$COUNT" in ''|*[!0-9]*) fail "checkpoint count='$COUNT' is not a number";; esac
[ "$COUNT" -ge 1 ] || fail "checkpoint count=$COUNT (want at least the gate record)"

captured=0
for _ in $(seq 1 120); do
    if grep -aqF "[ENV_PAYLOAD] captured partition=$PID index=1" "$L1"; then
        captured=1; break
    fi
    grep -aqF "[ENV_PAYLOAD] capture refused" "$L1" && \
        fail "the capture refused the healthy std::thread group: $(grep -aF 'capture refused' "$L1" | tail -1)"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the capture line"
    sleep 0.5
done
[ "$captured" -eq 1 ] || fail "the std::thread payload was never captured"
grep -aF "[ENV_PAYLOAD] linux group" "$L1" | tail -1 || fail "no group capture line"
if grep -aqF "EP_REFUSE_THREADS" "$L1"; then
    fail "a healthy two-thread group was refused with EP_REFUSE_THREADS"
fi

cleanup
echo "=== power off; the checkpoint and its payload are on the NVMe image ==="

echo "=== boot 2: restore, wake the worker, and join ==="
: > "$L2"
boot "$L2" two
BASE="http://127.0.0.1:$PORT"
api "$ART/restore.json" -X POST -d '{}' "$BASE/api/env/restore" || \
    fail "POST /api/env/restore did not answer (tail: $(tail -c 500 "$L2"))"
echo "restore: $(cat "$ART/restore.json")"
case "$(cat "$ART/restore.json")" in
    *'"ok":"true"'*) : ;;
    *) fail "the restore route refused the request: $(cat "$ART/restore.json")" ;;
esac
PAYLOADS=$(jval "$ART/restore.json" payloads)
SKIPS=$(jval "$ART/restore.json" payload_skips)
[ "$PAYLOADS" = "1" ] || fail "payloads=$PAYLOADS (want 1 — the group's bytes and contexts came back)"
[ "$SKIPS" = "0" ] || fail "payload_skips=$SKIPS (want 0)"
grep -aqF "[ENV_PAYLOAD] restored group" "$L2" || \
    fail "the restore did not recreate the group"
grep -aF "[ENV_PAYLOAD] restored group" "$L2" | tail -1
grep -aF "[ENV_PAYLOAD] thread" "$L2" | tail -4

ok=0
for _ in $(seq 1 480); do
    if grep -aqF "STD-THREAD-PASS" "$L2"; then ok=1; break; fi
    grep -aqF "STD-THREAD-FAIL" "$L2" && fail "the restored std::thread probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the restored std::thread gate completed"
    sleep 0.5
done
[ "$ok" -eq 1 ] || fail "the restored std::thread gate never reached STD-THREAD-PASS"

for marker in STD-PARENT-WAKE=-1 STD-FUTEX-WAKE=1 STD-WORKER-WOKE \
              STD-SHARED=7 STD-JOIN-OK STD-THREAD-PASS; do
    grep -aqF "$marker" "$L2" || fail "missing restored std::thread marker: $marker"
done
if grep -aqF "[FAULT]" "$L2"; then fail "a fault appeared in the restored std::thread log"; fi
if grep -aqF "STD-THREAD-FAIL" "$L2"; then fail "the restored std::thread gate failed"; fi
grep -aF "STD-" "$L2" | tail -10

cleanup
echo "PASS: std::thread::spawn used musl pthread_create's PROT_NONE stack"
echo "      reservation and mprotect, the group survived checkpoint/restore,"
echo "      and the restored JoinHandle completed with STD-THREAD-PASS."
echo "artifacts: $ART"

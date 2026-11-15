#!/usr/bin/env bash
# m4_ckpt_probe_check.sh — M4.6 increment 1's live checkpoint refusal.
#
# The gate profile is the only Linux tenant whose task name is already in an
# EnvCkptRecord. This build swaps that tenant for fixtures/m4_ckpt_probe.S:
# the parent clones a thread, waits for the child's write(1) to be answered,
# writes M4.6-CKPT-READY, and leaves BOTH threads spinning. The harness then
# POSTs /api/checkpoint. The kernel must:
#
#   1. answer the checkpoint (status 0) — the record was written;
#   2. name the live group: "capture refused (EP_REFUSE_THREADS)" and
#      "has 2 live thread(s)"; and
#   3. write NO payload: no "captured partition=... index=1" line and no
#      "capture: task ... m4.ckpt.probe.0" line for this environment.
#
# The last clause is the point of the probe. A capture that silently saved
# only the leader would also leave the checkpoint answer green; the absence of
# the payload lines, next to the refusal, is what distinguishes the two.
#
# Usage:
#   bash tests/m4_ckpt_probe_check.sh                 # build + live boot
#   M4_CKPT_BUILD=0 bash ...                          # use an existing image
#   M4_CKPT_ARTIFACTS=/tmp/dir bash ...               # keep evidence
#
# Exit 0 only after every clause is observed. The image is built with the
# feature profile and must not be mistaken for the ordinary E7 gate image:
#   E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe M4_CKPT_PROBE=1

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M4_CKPT_ISO:-sls_m4_ckpt.iso}"
BUILD="${M4_CKPT_BUILD:-1}"
ART="${M4_CKPT_ARTIFACTS:-/tmp/m4-ckpt-probe-$$}"
LOG="$ART/boot.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M4_CKPT_ENTRY:-3}"

mkdir -p "$ART" || { echo "ABORT: cannot create $ART" >&2; exit 2; }

QPID=""
CATPID=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${CATPID:-}" ] && kill "$CATPID" 2>/dev/null
    return 0
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }

if [ "$BUILD" = "1" ]; then
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe M4_CKPT_PROBE=1 X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe \
        M4_CKPT_PROBE=1 X86_E3_ISO="$ISO" || fail "the probe image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M4_CKPT_BUILD=0 needs a prebuilt one)"

# Source-level clauses: the task name is one constant, the fixture is the
# committed bytes the module includes, and the capture checks the group's
# live and address-space reference counts.
grep -qF 'pub const M4_CKPT_TASK_NAME: &str = "m4.ckpt.probe.0"' \
    user/proto/src/env_proto.rs || fail "the probe task name constant is missing"
grep -qF 'aerosls_proto::env_proto::M4_CKPT_TASK_NAME' user/init/src/env_manager.rs || \
    fail "init does not register the probe task name"
grep -qF 'crate::m4_ckpt_probe::spawn' user/sidecar/src/entry.rs || \
    fail "the gate profile does not select the probe spawn"
grep -qF 'linux_thread_group_refs(pds[t]->linux_tgid)' kernel/env_payload.c || \
    fail "the capture does not check the group's address-space refs"
[ -s user/sidecar/fixtures/m4_ckpt_probe.flat ] || fail "the probe fixture is missing"

rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"

PORT="${M4_CKPT_PORT:-$(bash tests/free_port.sh)}" || exit 2
SER="$ART/ser"
rm -f "$SER.in" "$SER.out"
mkfifo "$SER.in" "$SER.out" || fail "could not make the serial pipes"
cat "$SER.out" > "$LOG" &
CATPID=$!

qemu-system-x86_64 -cdrom "$ROOT/$ISO" \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
    -display none -m 1G -smp 2 -boot d -monitor none \
    -accel tcg,thread=multi \
    -serial pipe:"$SER" 2>"$ART/qemu.err" &
QPID=$!

bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || \
    fail "grub entry $ENTRY did not boot"

echo "=== boot: waiting for the control plane ==="
ready=0
for _ in $(seq 1 480); do
    if grep -aq "unified=1" "$LOG" 2>/dev/null && \
       grep -aq "Listening on port 3000" "$LOG" 2>/dev/null && \
       [ "$(grep -acF '[INIT] heartbeat' "$LOG")" -ge 5 ]; then
        ready=1
        break
    fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the control plane was up"
    sleep 0.5
done
[ "$ready" -eq 1 ] || fail "the unified boot did not reach its markers"
BASE="http://127.0.0.1:$PORT"
echo "up: control plane on $PORT"

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

echo "=== create the checkpointable environment ==="
api "$ART/pcreate.json" -X POST -d '{"name":"m4ckpt"}' "$BASE/api/partitions" || \
    fail "POST /api/partitions"
PID=$(jval "$ART/pcreate.json" partition_id)
[ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
echo "partition=$PID"
api "$ART/create.json" -X POST -d '{"index":1}' "$BASE/api/partition/$PID/env" || \
    fail "POST env"
echo "create: $(cat "$ART/create.json")"

echo "=== wait for the two-thread group ==="
spawned=0
for _ in $(seq 1 240); do
    if grep -aqF "[E7] firstuser: spawned" "$LOG"; then
        spawned=1
        break
    fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the probe spawned"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the probe tenant never spawned"

ready_group=0
for _ in $(seq 1 480); do
    if grep -aqF "M4.6-CKPT-READY" "$LOG"; then
        ready_group=1
        break
    fi
    grep -aqF "M4.6-CKPT-FAIL" "$LOG" && fail "the probe reported a clone failure"
    grep -aqF "M4.6-CHILD-FAIL" "$LOG" && fail "the probe's child write failed"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before M4.6-CKPT-READY"
    sleep 0.5
done
[ "$ready_group" -eq 1 ] || fail "the probe never reached M4.6-CKPT-READY"
grep -aqF "M4.6-THREAD-OK" "$LOG" || fail "the child's M4.6-THREAD-OK never appeared"
echo "--- probe markers:"
grep -aF "M4.6-" "$LOG" | tail -5

echo "=== checkpoint the live group ==="
api "$ART/checkpoint.json" -X POST "$BASE/api/checkpoint" || \
    fail "POST /api/checkpoint"
echo "checkpoint: $(cat "$ART/checkpoint.json")"
STATUS=$(jval "$ART/checkpoint.json" status)
COUNT=$(jval "$ART/checkpoint.json" count)
[ "$STATUS" = "0" ] || fail "checkpoint status=$STATUS (want 0)"
case "$COUNT" in
    ''|*[!0-9]*) fail "checkpoint count='$COUNT' is not a number" ;;
esac
[ "$COUNT" -ge 1 ] || fail "checkpoint count=$COUNT (want at least the gate record)"

echo "=== wait for the capture refusal ==="
refused=0
for _ in $(seq 1 120); do
    if grep -aqF "[ENV_PAYLOAD] capture refused (EP_REFUSE_THREADS)" "$LOG"; then
        refused=1
        break
    fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the capture refusal"
    sleep 0.5
done
[ "$refused" -eq 1 ] || fail "EP_REFUSE_THREADS was never reported"
REF=$(grep -aF "[ENV_PAYLOAD] capture refused (EP_REFUSE_THREADS)" "$LOG" | tail -1)
case "$REF" in
    *"m4.ckpt.probe.0' has 2 live thread(s)"*) : ;;
    *) fail "the refusal does not name the two-thread probe: $REF" ;;
esac
echo "--- refusal: $REF"

# The no-payload half. The capture logs a per-task line only after the group
# check; a "captured partition" line is the committed payload. Neither may
# mention this environment.
if grep -aqF "[ENV_PAYLOAD] captured partition=$PID index=1" "$LOG"; then
    fail "the checkpoint wrote a payload for the refused two-thread environment"
fi
if grep -aF "[ENV_PAYLOAD] capture: task" "$LOG" | grep -aqF "m4.ckpt.probe.0"; then
    fail "the checkpoint captured the probe task despite the refusal"
fi
grep -aqF "M4.6-CKPT-FAIL" "$LOG" && fail "the probe failed after the checkpoint"
grep -aqF "M4.6-CHILD-FAIL" "$LOG" && fail "the probe child failed after the checkpoint"
kill -0 "$QPID" 2>/dev/null || fail "QEMU exited during the checkpoint"

echo "=== power off ==="
cleanup
QPID=""
CATPID=""

echo "PASS: two-thread group created, checkpoint answered status=0,"
echo "      EP_REFUSE_THREADS reported for m4.ckpt.probe.0, and no payload"
echo "      for partition $PID index 1 was captured."
echo "artifacts: $ART"

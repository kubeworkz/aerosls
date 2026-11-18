#!/usr/bin/env bash
# tests/m5_tenant_ckpt_probe_check.sh — M5.3's ordinary-tenant Linux
# checkpoint/restore arm.
#
# The tenant profile is environment index 3, with the M5.2a tenant manifest bit
# and no `e7_gate`. This build swaps the short-lived catalog probe for the
# proven M4.6 two-thread fixture under `m5.tenant.ckpt.0`: the leader clones a
# thread, the child parks in an untimed private FUTEX_WAIT, the parent parks in
# a timed private FUTEX_WAIT, and the harness checkpoints only after both
# sidecars have logged `wait (futex)`.
#
# Boot 2 replays the record through the ordinary restore path, pours the
# payload, and the captured parent timeout wakes the parent. The parent wakes
# the child's restored untimed waiter; the child writes M4.6-FUTEX-WOKE and the
# parent writes M4.6-RESTORED-OK. A tenant task that was not captured, a task
# name that does not match init's record, or a lost waiter leaves the harness
# red.
#
# Usage:
#   bash tests/m5_tenant_ckpt_probe_check.sh              # build + two boots
#   M53_BUILD=0 bash ...                                 # use an existing image
#   M53_ARTIFACTS=/tmp/dir bash ...                      # keep evidence
#
# The image is built with:
#   E3_INIT_FEATURES=e3_envs,m5_tenant_probe,m5_tenant_ckpt M5_TENANT_CKPT=1

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M53_ISO:-sls_operating_system_e3_m53.iso}"
BUILD="${M53_BUILD:-1}"
ART="${M53_ARTIFACTS:-/tmp/m53-tenant-ckpt-$$}"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M53_ENTRY:-3}"
ENV_INDEX=3

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

if [ "$BUILD" = "1" ]; then
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,m5_tenant_probe,m5_tenant_ckpt M5_TENANT_CKPT=1 X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,m5_tenant_probe,m5_tenant_ckpt \
        M5_TENANT_CKPT=1 X86_E3_ISO="$ISO" || fail "the M5.3 image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M53_BUILD=0 needs a prebuilt one)"

# The task name is one constant, init names the record slot, and the sidecar
# selects the tenant checkpoint spawn. A drift in any half leaves the record
# naming a task the sidecar never spawns.
grep -qF 'pub const TENANT_CKPT_TASK_NAME: &str = "m5.tenant.ckpt.0"' \
    user/proto/src/env_proto.rs || fail "the tenant checkpoint task constant is missing"
grep -qF 'TENANT_CKPT_TASK_NAME' user/init/src/env_manager.rs || \
    fail "init does not register the tenant checkpoint task"
grep -qF 'm5_tenant_ckpt' user/init/Cargo.toml || \
    fail "init has no m5_tenant_ckpt feature"
grep -qF 'm5_tenant_ckpt' user/sidecar/Cargo.toml || \
    fail "the sidecar has no m5_tenant_ckpt feature"
grep -qF 'crate::m4_ckpt_probe::spawn_tenant' user/sidecar/src/entry.rs || \
    fail "the sidecar does not select the tenant checkpoint spawn"
[ -s user/sidecar/fixtures/m4_ckpt_probe.flat ] || \
    fail "the two-thread checkpoint fixture is missing"

rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"

boot() {   # boot <log> <tag>
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

echo "=== boot 1: create the ordinary tenant, park both waiters, checkpoint ==="
: > "$L1"
boot "$L1" one
BASE="http://127.0.0.1:$PORT"

api "$ART/pcreate.json" -X POST -d '{"name":"m53tenant"}' "$BASE/api/partitions" || \
    fail "POST /api/partitions"
PID=$(jval "$ART/pcreate.json" partition_id)
[ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
echo "partition=$PID"
api "$ART/create.json" -X POST -d '{"index":3}' "$BASE/api/partition/$PID/env" || \
    fail "POST env index 3"
ENV_ID=$(jval "$ART/create.json" env_id)
[ -n "$ENV_ID" ] && [ "$ENV_ID" != "0" ] || fail "no tenant env id"
echo "tenant env=$ENV_ID (index $ENV_INDEX)"

spawned=0
for _ in $(seq 1 300); do
    if grep -aqF "[M5.3] tenant ckpt" "$L1" && \
       grep -aqF "M4.6-CKPT-READY" "$L1" && \
       [ "$(grep -acF 'wait (futex)' "$L1")" -ge 2 ]; then
        spawned=1; break
    fi
    grep -aqF "M4.6-CKPT-FAIL" "$L1" && fail "the tenant checkpoint probe reported a failure"
    grep -aqF "M4.6-CHILD-FAIL" "$L1" && fail "the tenant checkpoint probe's child failed"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the tenant probe parked"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the tenant probe never reached READY with both futex waits parked"
grep -aF "M5.3]" "$L1" | tail -5
grep -aF "M4.6-" "$L1" | tail -5
echo "--- futex waits: $(grep -acF 'wait (futex)' "$L1")"

stamped=0
for _ in $(seq 1 120); do
    if grep -aqF "[ENV_CKPT] linux task 'm5.tenant.ckpt.0' stamped" "$L1"; then
        stamped=1; break
    fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the tenant task stamp"
    sleep 0.5
done
[ "$stamped" -eq 1 ] || fail "the tenant checkpoint task was never stamped into the record"
grep -aF "stamped" "$L1" | tail -1

api "$ART/checkpoint.json" -X POST "$BASE/api/checkpoint" || \
    fail "POST /api/checkpoint"
echo "checkpoint: $(cat "$ART/checkpoint.json")"
STATUS=$(jval "$ART/checkpoint.json" status)
COUNT=$(jval "$ART/checkpoint.json" count)
[ "$STATUS" = "0" ] || fail "checkpoint status=$STATUS (want 0)"
case "$COUNT" in ''|*[!0-9]*) fail "checkpoint count='$COUNT' is not a number";; esac
[ "$COUNT" -ge 1 ] || fail "checkpoint count=$COUNT (want at least the tenant record)"

captured=0
for _ in $(seq 1 120); do
    if grep -aqF "[ENV_PAYLOAD] captured partition=$PID index=$ENV_INDEX" "$L1"; then
        captured=1; break
    fi
    grep -aqF "[ENV_PAYLOAD] capture refused" "$L1" && \
        fail "the capture refused the healthy tenant group: $(grep -aF 'capture refused' "$L1" | tail -1)"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the tenant capture line"
    sleep 0.5
done
[ "$captured" -eq 1 ] || fail "the tenant payload was never captured"
grep -aF "[ENV_PAYLOAD] linux group" "$L1" | tail -1 || fail "no tenant group capture line"
if grep -aqF "EP_REFUSE_THREADS" "$L1"; then
    fail "the healthy tenant group was refused with EP_REFUSE_THREADS"
fi

echo "=== power off; the tenant checkpoint and payload are on the NVMe image ==="
cleanup

echo "=== boot 2: adopt, replay the tenant, pour, and wake the restored waiter ==="
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
REPLAYED=$(jval "$ART/restore.json" replayed)
REFUSED=$(jval "$ART/restore.json" refused)
[ "$PAYLOADS" = "1" ] || fail "payloads=$PAYLOADS (want 1 — the tenant's bytes and contexts came back)"
[ "$SKIPS" = "0" ] || fail "payload_skips=$SKIPS (want 0)"
[ "$REPLAYED" = "1" ] || fail "replayed=$REPLAYED (want 1)"
[ "$REFUSED" = "0" ] || fail "refused=$REFUSED (want 0)"

grep -aqF "[ENV_PAYLOAD] restored group" "$L2" || \
    fail "the restore did not recreate the tenant group"
grep -aqF "[ENV_PAYLOAD] thread 'm5.tenant.ckpt.0' restored PARKED" "$L2" || \
    fail "the restored tenant leader did not retain its parked futex form"
grep -aqF "[ENV_PAYLOAD] thread 'm5.tenant.ckpt.0.t106' restored PARKED" "$L2" || \
    fail "the restored tenant child did not retain its parked futex form"
grep -aqF "[PARTITION] partition 1 resumed" "$L2" || \
    fail "the tenant partition was not resumed after the restore pour"
grep -aF "[ENV_PAYLOAD] restored group" "$L2" | tail -1
grep -aF "[ENV_PAYLOAD] thread" "$L2" | tail -4

ok=0
for _ in $(seq 1 360); do
    if grep -aqF "M4.6-RESTORED-OK" "$L2"; then ok=1; break; fi
    grep -aqF "M4.6-CKPT-FAIL" "$L2" && fail "the tenant probe reported a failure after restore"
    grep -aqF "M4.6-CHILD-FAIL" "$L2" && fail "the tenant child failed after restore"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the restored tenant woke"
    sleep 0.5
done
[ "$ok" -eq 1 ] || fail "the tenant's parked futex waiter never survived the reboot (no M4.6-RESTORED-OK)"
grep -aqF "M4.6-FUTEX-PARENT-WAKE" "$L2" || fail "the restored parent never resumed from its captured futex wait"
grep -aqF "M4.6-FUTEX-WOKE" "$L2" || fail "the child's restored untimed futex waiter was never woken"
grep -aqF "M4.6-RESTORED-OK" "$L2" || fail "the restored parent never observed the child's wake"
if grep -aqF "M4.6-CKPT-FAIL" "$L2" || grep -aqF "M4.6-CHILD-FAIL" "$L2"; then
    fail "the restored tenant workload reported a failure"
fi
if grep -aqF "[E7-GATE]" "$L1" || grep -aqF "[E7-GATE]" "$L2"; then
    fail "the tenant checkpoint arm produced a gate marker"
fi
echo "--- tenant probe markers after restore:"
grep -aF "M4.6-" "$L2" | tail -6

cleanup
echo "PASS: ordinary tenant Linux task captured, payload restored, and the"
echo "      parked futex waiter woke after the reboot (M4.6-FUTEX-WOKE -> M4.6-RESTORED-OK)."
echo "artifacts: $ART"

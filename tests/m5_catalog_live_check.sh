#!/usr/bin/env bash
# tests/m5_catalog_live_check.sh — M5.2/M5.2a's live catalog probe proof.
#
# One QEMU boot of the E3 image built with M5_CATALOG_PROBE=1. The harness
# creates a partition, a bob-owned table, a dave-owned table with a known
# row, and a foreign table in PARTITION_SYSTEM, then creates the selected
# environment AS BOB. In the default gate arm that is environment index 1 and
# the sidecar spawns `m5.linux.catalog.0`; with M52_PROFILE=tenant it is an
# ordinary tenant environment index 3 with no FLAG_GATE, and the sidecar
# spawns `m5.tenant.catalog.0`. The flat fixture issues SYS_SLS_CATALOG_EXEC
# (334):
#
#   INSERT bob   -> success, then UPDATE bob -> success
#   DROP bob     -> -EINVAL (DDL is outside the first cut)
#   INSERT dave  -> -EPERM (not the channel's owner)
#   INSERT foreign -> -ENOENT (outside the partition)
#
# The fixture prints M52-CATALOG-OK only if every answer matched. The native
# observer is dave's shell: it must see bob's committed 'updated' row, and
# dave's own row must still be 'untouched' (a refusal that executed cannot
# pass). The foreign name must exist in GET /api/objects, so its ENOENT is a
# boundary answer rather than a missing table.
#
# GUARD-KIND: runtime (make + QEMU + a live boot). The source guards are
# e7_gate_check.sh and linux_personality_check.sh; this is the live arm.
#
# Knobs:
#   M52_PROFILE        gate (default) or tenant (M5.2a ordinary tenant)
#   M52_BUILD=1        build the image (default); 0 reuses M52_ISO
#   M52_ISO            default sls_operating_system_e3_m52.iso for gate,
#                      sls_operating_system_e3_m52a.iso for tenant
#   M52_BOOT_ENTRY     default 3
#   M52_SPAWN_WAIT_S   default 300
#   M52_TOKEN          default the tree's fixed dev token
#   M52_SMP            QEMU -smp count (default 1). The CAT_EXEC write path
#                      enters the synchronous row-store persistence path; on
#                      this tree a two-vCPU boot exposed a pre-existing kernel
#                      fault outside this syscall that the one-vCPU run does
#                      not reproduce. Pin one vCPU here so the live arm
#                      measures the catalog boundary, not that fault. Override
#                      with M52_SMP=2 to reproduce it.
#   M52_ARTIFACTS      default /tmp/m52-catalog-$$
set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"

PROFILE="${M52_PROFILE:-gate}"
case "$PROFILE" in
    gate)   DEFAULT_ISO=sls_operating_system_e3_m52.iso;  ENV_INDEX=1 ;;
    tenant) DEFAULT_ISO=sls_operating_system_e3_m52a.iso; ENV_INDEX=3 ;;
    *) echo "ABORT: M52_PROFILE must be 'gate' or 'tenant', got '$PROFILE'" >&2; exit 2 ;;
esac
ISO="${M52_ISO:-$DEFAULT_ISO}"
BUILD="${M52_BUILD:-1}"
ENTRY="${M52_BOOT_ENTRY:-3}"
SPAWN_WAIT="${M52_SPAWN_WAIT_S:-300}"
SMP="${M52_SMP:-1}"
TOKEN="${M52_TOKEN:-deadbeef01234567cafebabe76543210}"
PEER_TOKEN="${M52_PEER_TOKEN:-cafebabe7654321089abcdef01234567}"
BOB_UID=1001
ART="${M52_ARTIFACTS:-/tmp/m52-catalog-$$}"
WD="$ART/work"
LOG="$ART/boot.log"
IMG="$ART/nvme.img"

command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not found" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "ABORT: curl not found" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
mkdir -p "$ART" "$WD" || { echo "ABORT: cannot create $ART" >&2; exit 2; }

if [ "$BUILD" = 1 ]; then
    if [ "$PROFILE" = gate ]; then
        BUILD_FEATURES=e3_envs,e7_gate
    else
        BUILD_FEATURES=e3_envs,m5_tenant_probe
    fi
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=$BUILD_FEATURES M5_CATALOG_PROBE=1 X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES="$BUILD_FEATURES" M5_CATALOG_PROBE=1 \
        X86_E3_ISO="$ISO" || { echo "ABORT: the M5.2 image did not build" >&2; exit 2; }
fi
[ -f "$ROOT/$ISO" ] || { echo "ABORT: no image at $ISO (M52_BUILD=0 needs a prebuilt one)" >&2; exit 2; }

QPID=""; CATPID=""; PORT=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${CATPID:-}" ] && kill "$CATPID" 2>/dev/null
    return 0
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }

echo "=== boot: $ISO (grub entry $ENTRY) ==="
PORT=$(bash tests/free_port.sh) || fail "no free port"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"
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
    -display none -m 1G -smp "$SMP" -boot d -monitor none \
    -accel tcg,thread=multi \
    -serial pipe:"$SER" 2>"$ART/qemu.err" &
QPID=$!
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || \
    fail "grub entry $ENTRY did not boot"
ready=0
for _ in $(seq 1 480); do
    if grep -aq "unified=1" "$LOG" 2>/dev/null && \
       grep -aq "Listening on port 3000" "$LOG" 2>/dev/null && \
       [ "$(grep -acF '[INIT] heartbeat' "$LOG")" -ge 5 ]; then
        ready=1; break
    fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the control plane was up"
    sleep 0.5
done
[ "$ready" = 1 ] || fail "the boot did not reach its control-plane markers"
BASE="http://127.0.0.1:$PORT"
echo "up: control plane on $PORT"

# ─── HTTP helpers ──────────────────────────────────────────────────────────
api() {   # $1 = method, $2 = path, $3 = optional body; sets API_BODY/API_CODE
    local method="$1" path="$2" body="${3:-}" out
    if [ -n "$body" ]; then
        out=$(curl -s --max-time 120 -w '\n%{http_code}' \
            -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
            -X "$method" -d "$body" "$BASE$path" 2>/dev/null)
    else
        out=$(curl -s --max-time 120 -w '\n%{http_code}' \
            -H "Authorization: Bearer $TOKEN" -X "$method" "$BASE$path" 2>/dev/null)
    fi
    API_CODE=$(printf '%s' "$out" | tail -n1)
    API_BODY=$(printf '%s' "$out" | sed '$d')
}
jval() { python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get(sys.argv[2],""))' "$1" "$2" 2>/dev/null; }

SHEL_OUT=""; SHEL_RECOG=""
sh_exec_as() {   # $1 = token, $2 = command
    local tok="$1" cmd="$2" payload out
    payload=$(python3 -c 'import json,sys; print(json.dumps({"command":sys.argv[1]}))' "$cmd")
    out=$(curl -s --max-time 120 -w '\n%{http_code}' \
        -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
        -X POST -d "$payload" "$BASE/api/shell/exec" 2>/dev/null)
    API_CODE=$(printf '%s' "$out" | tail -n1)
    API_BODY=$(printf '%s' "$out" | sed '$d')
    SHEL_RECOG=$(jval "$API_BODY" recognized)
    SHEL_OUT=$(python3 -c 'import json,sys
try: d=json.loads(sys.argv[1])
except Exception: d={}
print((d.get("output") or "").replace("\r",""), end="")' "$API_BODY" 2>/dev/null)
    [ "$API_CODE" = 200 ] && [ "$SHEL_RECOG" != false ]
}
sh_exec() { sh_exec_as "$TOKEN" "$1"; }

wait_for() {   # $1 = fixed needle, $2 = timeout seconds, $3 = description
    local needle="$1" timeout="$2" what="$3" i
    for i in $(seq 1 $((timeout * 2))); do
        grep -aqF "$needle" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.5
    done
    echo "  (last M5.2 lines:)" >&2
    grep -aF '[M5.2]' "$LOG" | tail -10 | sed 's/^/    /' >&2
    echo "  ($what missing: '$needle')" >&2
    return 1
}

# ─── partition and the three tables ───────────────────────────────────────
RUN=$$
api POST /api/partitions "{\"name\":\"m52-$RUN\"}"
[ "$API_CODE" = 200 ] || fail "POST /api/partitions -> HTTP $API_CODE: $API_BODY"
PART=$(jval "$API_BODY" partition_id)
[ -n "$PART" ] && [ "$PART" != 0 ] || fail "partition create returned no id: $API_BODY"
echo "partition=$PART"

api GET /api/session/whoami
DAVE_UID=$(jval "$API_BODY" uid)
[ -n "$DAVE_UID" ] || fail "whoami returned no uid: $API_BODY"
api POST /api/partition/assign "{\"uid\":$DAVE_UID,\"partition_id\":$PART}"
[ "$API_CODE" = 200 ] || fail "assign dave uid $DAVE_UID: $API_BODY"
api POST /api/partition/assign "{\"uid\":$BOB_UID,\"partition_id\":$PART}"
[ "$API_CODE" = 200 ] || fail "assign bob uid $BOB_UID: $API_BODY"
echo "uids: dave=$DAVE_UID bob=$BOB_UID assigned to partition $PART"

# dave's table: the denial arm's row must remain 'untouched'.
sh_exec "valloc m52_dave 1 1" || fail "dave valloc: $SHEL_OUT"
sh_exec "schema set m52_dave note STRING" || fail "dave schema: $SHEL_OUT"
sh_exec "table create m52_dave" || fail "dave table create: $SHEL_OUT"
sh_exec "sql INSERT INTO m52_dave (note) VALUES ('untouched')" || fail "dave seed insert: $SHEL_OUT"
case "$SHEL_OUT" in *"[SQL] OK,"*) ;; *) fail "dave seed insert did not report OK: $SHEL_OUT" ;; esac
# The catalog's legacy default perm_mask carries WRITE, so APP_USER bob would
# pass the object-level fallback on dave's table without exercising the
# owner rule. Pin it to PERM_READ first; then a bob INSERT is decided by
# PERM_WRITE and must be refused. (M5.1's live arm does the same.)
api POST /api/chmod '{"name":"m52_dave","mask":"0x4"}'
[ "$API_CODE" = 200 ] || fail "chmod m52_dave to PERM_READ: HTTP $API_CODE: $API_BODY"

# bob's table: the successful writes target this owner.
sh_exec_as "$PEER_TOKEN" "valloc m52_bob 1 1" || fail "bob valloc: $SHEL_OUT"
sh_exec_as "$PEER_TOKEN" "schema set m52_bob note STRING" || fail "bob schema: $SHEL_OUT"
sh_exec_as "$PEER_TOKEN" "table create m52_bob" || fail "bob table create: $SHEL_OUT"
case "$SHEL_OUT" in *"create 'm52_bob' -> OK"*) ;; *) fail "bob table create did not report OK: $SHEL_OUT" ;; esac

# The foreign table is owned by uid 0 through the HTTP route, so it lands in
# PARTITION_SYSTEM. Its existence is checked independently below.
api POST /api/valloc '{"name":"m52_foreign","type":1,"pages":1}'
[ "$API_CODE" = 200 ] || fail "foreign valloc: HTTP $API_CODE: $API_BODY"
echo "tables: m52_dave (dave), m52_bob (bob), m52_foreign (partition system)"

# ─── the selected profile, created AS BOB ────────────────────────────────
sh_exec_as "$PEER_TOKEN" "env create $PART $ENV_INDEX" || \
    fail "bob 'env create $PART $ENV_INDEX': $SHEL_OUT"
ENV_ID=$(printf '%s' "$SHEL_OUT" | sed -n 's/.*env_id=\([0-9]*\).*/\1/p' | tail -1)
[ -n "$ENV_ID" ] && [ "$ENV_ID" != 0 ] || fail "bob env create returned no env_id: $SHEL_OUT"
echo "$PROFILE env=$ENV_ID (index $ENV_INDEX, owner bob)"

if [ "$PROFILE" = tenant ]; then
    SPAWN_NEEDLE='[M5.2] tenant probe: sp'
    SPAWN_WHAT="the ordinary tenant sidecar's M5.2 probe spawn"
else
    SPAWN_NEEDLE='[M5.2] catalog probe: sp'
    SPAWN_WHAT="the gate sidecar's M5.2 probe spawn"
fi
wait_for "$SPAWN_NEEDLE" 120 "$SPAWN_WHAT" || \
    fail "the sidecar never spawned the M5.2 probe"
wait_for "M52-CATALOG-OK" "$SPAWN_WAIT" "the probe's success marker" || \
    fail "the probe did not report M52-CATALOG-OK"
if grep -aqF "M52-CATALOG-FAIL" "$LOG"; then
    fail "the probe reported M52-CATALOG-FAIL"
fi
if [ "$PROFILE" = tenant ]; then
    # M5.2a is an ORDINARY tenant arm: no gate manifest, no gate marker, no
    # gate stop/checkpoint behavior. A leaked gate arm invalidates the claim.
    if grep -aqF "[E7-GATE]" "$LOG"; then
        fail "the tenant arm produced an [E7-GATE] marker"
    fi
    if grep -aqE "gate env=[0-9]+" "$LOG"; then
        fail "the tenant arm created a gate environment"
    fi
    echo "ok:   the tenant probe ran without the gate profile (no [E7-GATE])"
fi
echo "ok:   the Linux task issued 334 and got the promised answers (M52-CATALOG-OK)"

# ─── the native observer: dave's own shell, outside the mediated path ─────
sh_exec "sql SELECT * FROM m52_bob" || fail "native dave SELECT m52_bob: $SHEL_OUT"
printf '%s\n' "$SHEL_OUT" | grep -Fq "updated" || \
    fail "dave's native read does not see bob's committed 'updated' row: $SHEL_OUT"
echo "ok:   native observer sees bob's committed UPDATE ('updated')"

sh_exec "sql SELECT * FROM m52_dave" || fail "native dave SELECT m52_dave: $SHEL_OUT"
printf '%s\n' "$SHEL_OUT" | grep -Fq "untouched" || \
    fail "dave's own row disappeared: $SHEL_OUT"
if printf '%s\n' "$SHEL_OUT" | grep -Fq "denied"; then
    fail "the denied INSERT into dave's table executed: $SHEL_OUT"
fi
echo "ok:   dave's row is unchanged ('untouched', no 'denied') — the EPERM refusal did not execute"

api GET /api/objects
printf '%s' "$API_BODY" | grep -Fq '"m52_foreign"' || \
    fail "the foreign table is not in GET /api/objects: $API_BODY"
echo "ok:   the foreign table exists in the catalog; its ENOENT was the partition boundary"

echo "M5.2 live $PROFILE profile: PASS (partition=$PART env=$ENV_ID)"
echo "artifacts: $ART"

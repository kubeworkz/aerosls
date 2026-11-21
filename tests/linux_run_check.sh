#!/usr/bin/env bash
# tests/linux_run_check.sh — step 1 of tenant Linux: an ordinary tenant uploads
# a MAX_IMAGE-padded static ELF into its own VFS and spawns it through the real
# kernel, while the child's VFS task is allocated only after a successful
# linux_spawn. A second `linux run` must be refused by the one-spawn limit.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# Before this step the only tenant Linux spawns were build-time probes with
# embedded fixtures. The interesting properties of the shell path are all
# properties of the SEAM, not of the fixture: the console line carrying the
# upload is 481 bytes and used to hit a 256-byte sidecar receive buffer
# (CAP_ERR_BUFSZ, the message left queued, the environment wedged); the shell
# reads 16-byte console chunks and buffers ahead, so an uploader that starts
# streaming hex before HEXWRITE-READY loses its first bytes; Vfs::add_task has
# no removal, so a failed spawn that allocated a child task leaks a slot; and a
# child task allocated before linux_spawn can hand its fd table to a process
# the kernel never created. This guard boots the existing M5.2a ordinary
# tenant test profile (profile 0, index 3), drives those exact paths through
# the tenant's authenticated shell, and asserts the seam held. A plain E3
# default create currently mounts an unseeded inode image
# (MountRefused("magic")) before step 1 is reached; the M5.2a shell-create
# path is the working tenant route in this tree. Step 1
# itself stays sidecar-only: no profile selector, no registration flag.
#
# ─── What it asserts ───────────────────────────────────────────────────────
#   1. The unified boot reaches its control plane and a tenant environment is
#      created through the authenticated shell's `env create` with the default
#      profile (no step 2 profile selector).
#   2. Its shell answers on its own console; `linux run` of a missing payload
#      is refused by name, without consuming the one-shot.
#   3. A dynamically linked ELF (PT_INTERP) uploaded through `hexwrite` is
#      refused by name: step 1 accepts static ET_EXEC only.
#   4. A static ET_EXEC padded to exactly MAX_IMAGE (64 KiB) is uploaded the
#      same way (240-byte chunks, each acked with HEX-OK), executes and prints
#      TENANT-EXEC-OK; `linux run` reports LINUX-RUN-OK checkpoint=none. A
#      second `linux run` is refused `LINUX-RUN-ERR already-run`; no second
#      child executes.
#   5. The kernel stamped no checkpoint Linux slot for `linux.run.0`: the
#      default tenant record names only tasks 0/1, so the child stays outside
#      the step 2 checkpoint boundary.
#   6. Profile 1 (`tenant-linux-ckpt`) is refused as unsupported in this image.
#   7. The tenant environment is destroyed cleanly over the E5 route.
#
# ─── Build ─────────────────────────────────────────────────────────────────
#   make x86-iso-e3 LINUX_RUN=1 M5_CATALOG_PROBE=1 \
#        E3_INIT_FEATURES=e3_envs,m5_tenant_probe \
#        SIDECAR_E3_CPIO=sidecars_linux_run.cpio \
#        X86_E3_ISO=sls_linux_run.iso
#
# The image carries the sidecar `linux_run` feature and the existing M5.2a
# ordinary-tenant test profile (init `m5_tenant_probe`, sidecar
# `m5_catalog_probe`); it deliberately does NOT carry `e7_gate` or the step 2
# `m5_tenant_ckpt` profile. The gate compiles its small static fixture with the
# host `cc` into a temp directory and pads it to exactly MAX_IMAGE (64 KiB);
# the dynamic refusal uses the committed M2d fixture.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Env knobs:
#   LR_ISO           path to the ISO                 (default sls_linux_run.iso)
#   LR_BOOT_ENTRY    1-based grub menu position      (default 3 = unified)
#   LR_WINDOW_S      boot marker wait                (default 180)
#   LR_ATTACH_WAIT_S console wait per phase           (default 45)
#   LR_SMP           QEMU -smp                       (default 4)
#   LR_DYN_ELF       dynamic fixture                 (default user/sidecar/fixtures/m2_dyn_hello.elf)
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first), 2 on a
# missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${LR_ISO:-sls_linux_run.iso}"
LOG=/tmp/aerosls_linux_run_boot.log
ENTRY="${LR_BOOT_ENTRY:-3}"
WINDOW_S="${LR_WINDOW_S:-180}"
ATTACH_WAIT_S="${LR_ATTACH_WAIT_S:-45}"
SMP="${LR_SMP:-4}"
DYN_ELF="${LR_DYN_ELF:-user/sidecar/fixtures/m2_dyn_hello.elf}"
STATIC_SRC=tests/fixtures/linux_run_static.S

[ -f "$ISO" ] || {
    echo "ABORT: $ISO missing — build it with:" >&2
    echo "       make x86-iso-e3 LINUX_RUN=1 M5_CATALOG_PROBE=1 E3_INIT_FEATURES=e3_envs,m5_tenant_probe SIDECAR_E3_CPIO=sidecars_linux_run.cpio X86_E3_ISO=sls_linux_run.iso" >&2
    exit 2
}
[ -f "$STATIC_SRC" ] || { echo "ABORT: $STATIC_SRC missing" >&2; exit 2; }
[ -f "$DYN_ELF" ] || { echo "ABORT: $DYN_ELF missing (the PT_INTERP refusal fixture)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }
command -v cc >/dev/null 2>&1 || { echo "ABORT: cc not installed (static fixture build)" >&2; exit 2; }

# ── The two fixtures ────────────────────────────────────────────────────────
# The static fixture is deliberately tiny and raw-syscall: no libc, no startup
# objects, no PT_INTERP. `-static -no-pie` is what makes it an ET_EXEC the
# step 1 gate accepts; the dynamic fixture beside it is the refusal control.
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
STATIC_ELF="$W/linux_run_static.elf"
if ! cc -nostdlib -static -no-pie -o "$STATIC_ELF" "$STATIC_SRC" 2>"$W/cc.err"; then
    echo "ABORT: could not build the static fixture:" >&2
    sed 's/^/       /' "$W/cc.err" >&2
    exit 2
fi
[ -s "$STATIC_ELF" ] || { echo "ABORT: the static fixture is empty" >&2; exit 2; }
# Exercise the inclusive MAX_IMAGE cap: the payload must be exactly 64 KiB,
# not one byte less. Keep this in sync with linux_run::MAX_IMAGE.
MAX_IMAGE_BYTES=65536
truncate -s "$MAX_IMAGE_BYTES" "$STATIC_ELF" || {
    echo "ABORT: could not pad the static fixture to MAX_IMAGE" >&2; exit 2;
}
[ "$(wc -c < "$STATIC_ELF" | tr -d ' ')" = "$MAX_IMAGE_BYTES" ] || {
    echo "ABORT: the static fixture is not exactly MAX_IMAGE bytes" >&2; exit 2;
}

# ── QEMU + serial, the E6 attach shape ──────────────────────────────────────
SER=/tmp/aerosls_linux_run_ser
rm -f "$SER.in" "$SER.out" "$LOG"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd" >&2; exit 2; }
BASE="http://127.0.0.1:$PORT"
TOKEN=deadbeef01234567cafebabe76543210
# The M5.2a ordinary-tenant arm creates its environment as bob, whose fixed
# peer token is the tree's second dev identity. The env manager's rootfs seed
# follows that owner; creating as the DB_ADMIN caller instead leaves the E3
# tenant's block image unseeded. Console writes and teardown stay on dave.
PEER_TOKEN=cafebabe7654321089abcdef01234567
BOB_UID=1001

ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
        ACCEL="-accel kvm"
    else
        ACCEL="-accel tcg,thread=multi"
    fi
fi

qemu-system-x86_64 -cdrom "$ISO" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:08 \
    -display none -m 4G -smp "$SMP" -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>"$W/qemu.err" &
QPID=$!

cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; }
trap 'cleanup; rm -rf "$W"; exit 1' TERM INT

qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
    qemu_err
    cleanup
    exit 1
}

hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }

ready=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aq "\[BOOT\] command line:" "$LOG" 2>/dev/null &&
       ! grep -aq "unified=1" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY booted without unified=1 (not the unified boot)" >&2
        cleanup
        exit 1
    fi
    if grep -aq "Listening on port 3000" "$LOG" 2>/dev/null &&
       ! grep -aq "\[E1\] control plane planted" "$LOG" 2>/dev/null; then
        echo "FAILED: the HTTP listener came up without planting the Ring-0 control plane — the environment manager cannot be driven from this boot" >&2
        cleanup
        exit 1
    fi
    if [ "$(hb_now)" -ge 5 ] && grep -aq "Listening on port 3000" "$LOG" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        qemu_alive=0
        break
    fi
    sleep 0.5
done

fail=0
report_and_exit() {
    echo "FAIL  the tenant VFS-loaded static ELF spawn (step 1)" >&2
    echo "      ENV / POSIX / LINUX lines seen:" >&2
    grep -a "\[ENV\]\|\[POSIX\]\|\[LINUX\]\|\[LINUX-RUN\]\|HEXWRITE\|LINUX-RUN" "$LOG" 2>/dev/null | tail -40 | sed 's/^/      /' >&2
    echo "      last 25 serial lines:" >&2
    tail -25 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    echo "      console stream so far:" >&2
    printf '%s\n' "${S:-}" 2>/dev/null | tail -15 | sed 's/^/      /' >&2
    qemu_err
    cleanup
    exit 1
}

if [ "$ready" -ne 1 ]; then
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAILED: QEMU exited before the unified boot reached its markers — the boot crashed" >&2
    else
        echo "FAILED: boot markers missing within ${WINDOW_S}s (heartbeats: $(hb_now))" >&2
    fi
    report_and_exit
fi
echo "ok:   the unified boot is up (control plane serving, init making progress)"

# ── HTTP helpers ────────────────────────────────────────────────────────────
api() {   # api <out-file> <curl args...>
    local out="$1"; shift
    : > "$out"
    local t
    for t in 1 2 3 4 5; do
        if curl -sf --max-time 30 -H "Authorization: Bearer $TOKEN" \
                -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null; then
            return 0
        fi
        sleep 2
    done
    return 1
}
api_as() {   # api_as <token> <out-file> <curl args...>
    local tok="$1" out="$2"; shift 2
    : > "$out"
    local t
    for t in 1 2 3 4 5; do
        if curl -sf --max-time 30 -H "Authorization: Bearer $tok" \
                -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null; then
            return 0
        fi
        sleep 2
    done
    return 1
}
jval() {   # jval <json-file> <key>
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
v = d.get(sys.argv[2]) if isinstance(d, dict) else None
if v is None:            print("")
elif v is True:          print("true")
elif v is False:         print("false")
else:                    print(v)
PY
}
env_field() {   # env_field <envs.json> <env_id> <field>
    python3 - "$1" "$2" "$3" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
for e in d.get("envs", []):
    if str(e.get("env_id")) == sys.argv[2]:
        v = e.get(sys.argv[3])
        sys.stdout.write("" if v is None else str(v))
        break
PY
}
console_get() {   # console_get <partition> <env_id> — "<ok> <dropped>"; body in $W/cons.json
    local part="$1" id="$2"
    api "$W/cons.json" "$BASE/api/partition/$part/env/$id/console" || true
    printf '%s %s\n' "$(jval "$W/cons.json" ok)" "$(jval "$W/cons.json" dropped)"
}
console_out() {
    python3 - "$W/cons.json" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get("output") if isinstance(d, dict) else None
if isinstance(v, str):
    sys.stdout.write(v)
PY
}

S=""; D="0"
drain() {
    local r
    r="$(console_get "$pid" "$E")"
    C_OK="${r%% *}"
    C_DROP="${r##* }"
    if [ "$C_OK" = "true" ]; then
        S="$S$(console_out)"
        D="$C_DROP"
    fi
}
# Count a marker's occurrences in the stream accumulated so far. The upload
# ack count is the gate's proof that each 240-byte chunk was written before the
# next one was sent.
count_marker() { printf '%s' "$S" | grep -o -F -- "$1" 2>/dev/null | wc -l | tr -d ' '; }
wait_marker() {   # wait_marker <marker> <seconds> — substring
    local marker="$1" secs="$2" i
    for i in $(seq 1 $((secs * 2))); do
        drain
        case "$S" in *"$marker"*) return 0 ;; esac
        sleep 0.5
    done
    return 1
}
wait_count() {   # wait_count <marker> <wanted-count> <seconds>
    local marker="$1" want="$2" secs="$3" i
    for i in $(seq 1 $((secs * 2))); do
        drain
        if [ "$(count_marker "$marker")" -ge "$want" ]; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# The kernel refuses a new console line while the peer has not drained the
# previous one, so every send waits for a non-zero `queued` rather than
# assuming the line landed.
SEND_OK=""; SEND_QUEUED=""; SEND_ERR=""
send_raw() {
    local input="$1"
    api "$W/send.json" -X POST -d "{\"input\":\"$input\"}" \
        "$BASE/api/partition/$pid/env/$E/console" || true
    SEND_OK="$(jval "$W/send.json" ok)"
    SEND_QUEUED="$(jval "$W/send.json" queued)"
    SEND_ERR="$(jval "$W/send.json" error)"
}
send_line() {   # send_line <text> — wait until the queue accepts it
    local input="$1" t
    for t in $(seq 1 60); do
        send_raw "$input"
        if [ "$SEND_OK" = "true" ] && [ -n "$SEND_QUEUED" ] && [ "$SEND_QUEUED" != "0" ]; then
            return 0
        fi
        if [ "$SEND_OK" = "false" ]; then
            return 1
        fi
        sleep 0.5
    done
    return 1
}

# ── Phase 1: partition, tenant environment, live console ───────────────────
api "$W/health.json" "$BASE/api/health" || true
if [ "$(jval "$W/health.json" status)" = "ok" ]; then
    echo "ok:   /api/health answers with a live kernel body"
else
    echo "FAILED: /api/health did not answer a live body" >&2
    report_and_exit
fi

api "$W/pcreate.json" -X POST -d '{"name":"linuxrun"}' "$BASE/api/partitions" || true
pid="$(jval "$W/pcreate.json" partition_id)"
if [ "$(jval "$W/pcreate.json" ok)" = "true" ] && [ -n "$pid" ] && [ "$pid" != "0" ] && [ "$pid" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined the tenant's partition $pid"
else
    echo "FAILED: POST /api/partitions did not define a partition (id='$pid')" >&2
    report_and_exit
fi

# The E3 env manager seeds the tenant's rootfs into the partition's storage;
# the caller must own a share of that partition before the create, as the
# M5.2a gate does. Without this, the sidecar boots against an unseeded block
# image and panics MountRefused("magic") before step 1 runs.
api "$W/who.json" "$BASE/api/session/whoami" || true
CALLER_UID="$(jval "$W/who.json" uid)"
if [ -z "$CALLER_UID" ]; then
    echo "FAILED: /api/session/whoami returned no uid" >&2
    report_and_exit
fi
api "$W/assign.json" -X POST -d "{\"uid\":$CALLER_UID,\"partition_id\":$pid}" \
    "$BASE/api/partition/assign" || true
if [ "$(jval "$W/assign.json" ok)" = "false" ]; then
    echo "FAILED: could not assign the authenticated uid $CALLER_UID to partition $pid (error='$(jval "$W/assign.json" error)')" >&2
    report_and_exit
fi
echo "ok:   authenticated uid $CALLER_UID shares partition $pid"
api "$W/assign_bob.json" -X POST -d "{\"uid\":$BOB_UID,\"partition_id\":$pid}" \
    "$BASE/api/partition/assign" || true
if [ "$(jval "$W/assign_bob.json" ok)" = "false" ]; then
    echo "FAILED: could not assign bob (uid $BOB_UID) to partition $pid (error='$(jval "$W/assign_bob.json" error)')" >&2
    report_and_exit
fi
echo "ok:   bob (uid $BOB_UID) also shares partition $pid"

# The M5.2a working path creates the tenant through the authenticated shell's
# `env create` (the HTTP default-create route mounts an unseeded block image in
# this tree's E3 env manager). The console and the rest still use HTTP.
SHELL_OUT=""; SHELL_RECOG=""
shell_exec_as() {   # shell_exec_as <token> <command> — sets SHELL_OUT / SHELL_RECOG
    local tok="$1" cmd="$2" payload
    payload=$(python3 -c 'import json,sys; print(json.dumps({"command":sys.argv[1]}))' "$cmd")
    api_as "$tok" "$W/shell.json" -X POST -d "$payload" "$BASE/api/shell/exec" || true
    SHELL_RECOG="$(jval "$W/shell.json" recognized)"
    SHELL_OUT=$(python3 - "$W/shell.json" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    d = {}
print((d.get("output") or "").replace("\r", ""), end="")
PY
)
    [ "$SHELL_RECOG" != "false" ]
}

if ! shell_exec_as "$PEER_TOKEN" "env create $pid 3"; then
    echo "FAILED: bob's shell 'env create $pid 3' was not recognized (recognized='$SHELL_RECOG', output='$SHELL_OUT')" >&2
    report_and_exit
fi
E="$(printf '%s' "$SHELL_OUT" | sed -n 's/.*env_id=\([0-9]*\).*/\1/p' | tail -1)"
if [ -n "$E" ] && [ "$E" != "0" ]; then
    echo "ok:   environment 3 placed in partition $pid as bob (env_id $E, default profile; shell create)"
else
    echo "FAILED: bob's shell 'env create $pid 3' returned no env_id (output='$SHELL_OUT')" >&2
    report_and_exit
fi

bound=0
for i in $(seq 1 120); do
    api "$W/envs.json" "$BASE/api/partition/$pid/env" || true
    if [ "$(jval "$W/envs.json" live)" = "1" ] && [ "$(env_field "$W/envs.json" "$E" index)" = "3" ]; then
        bound=1
        break
    fi
    sleep 0.5
done
if [ "$bound" -ne 1 ]; then
    echo "FAILED: the environment's console never became live/bound (live='$(jval "$W/envs.json" live)', index='$(env_field "$W/envs.json" "$E" index)')" >&2
    report_and_exit
fi

if ! wait_marker "[env-id] index=3" "$ATTACH_WAIT_S"; then
    echo "FAILED: the tenant console never carried its own [env-id] announcement" >&2
    report_and_exit
fi
if ! wait_marker "$ " "$ATTACH_WAIT_S"; then
    echo "FAILED: the tenant shell never printed its prompt" >&2
    report_and_exit
fi
echo "ok:   the tenant console is live, announces index 3, and its shell is at a prompt"

# ── Phase 2: the missing payload is refused without consuming the one-shot ─
if ! send_line "linux run /tmp/linux_run_missing.elf"; then
    echo "FAILED: could not send the missing-payload command (error='$SEND_ERR')" >&2
    report_and_exit
fi
if wait_marker "LINUX-RUN-ERR missing payload" "$ATTACH_WAIT_S"; then
    echo "ok:   linux run of a missing payload is refused by name"
else
    echo "FAILED: linux run of a missing payload did not report 'missing payload'" >&2
    report_and_exit
fi

# ── Phase 3: hexwrite — ready first, then 240-byte chunks, each acked ───────
# The console line cap is 512 bytes (kernel/env_console.c), and 240 binary
# bytes are 480 hex characters. The applet acks every full chunk with HEX-OK;
# the gate waits for that ack before sending the next line, which is also what
# keeps the kernel's queue from refusing input.
hex_stream() {
    local file="$1"
    if command -v xxd >/dev/null 2>&1; then
        xxd -p -c 240 "$file"
    else
        od -An -v -tx1 "$file" | tr -d ' \n' | fold -w 480
    fi
}
upload() {   # upload <tenant-path> <host-file>
    local tpath="$1" file="$2" size chunks base n chunk ready_base done_base
    size=$(wc -c < "$file" | tr -d ' ')
    ready_base=$(count_marker "HEXWRITE-READY")
    if ! send_line "hexwrite $tpath $size"; then
        echo "FAILED: could not send the hexwrite command for $tpath (error='$SEND_ERR')" >&2
        return 1
    fi
    if ! wait_count "HEXWRITE-READY" $((ready_base + 1)) "$ATTACH_WAIT_S"; then
        echo "FAILED: hexwrite never reported a fresh HEXWRITE-READY for $tpath (the upload must wait for it)" >&2
        return 1
    fi
    mapfile -t chunks < <(hex_stream "$file")
    base=$(count_marker "HEX-OK")
    done_base=$(count_marker "HEXWRITE-OK")
    n=0
    for chunk in "${chunks[@]}"; do
        n=$((n + 1))
        if ! send_line "$chunk"; then
            echo "FAILED: could not send chunk $n of $tpath (error='$SEND_ERR')" >&2
            return 1
        fi
        if ! wait_count "HEX-OK" $((base + n)) "$ATTACH_WAIT_S"; then
            echo "FAILED: chunk $n of $tpath was not acked (HEX-OK count $(count_marker 'HEX-OK'), wanted $((base + n)))" >&2
            return 1
        fi
    done
    if ! wait_count "HEXWRITE-OK" $((done_base + 1)) "$ATTACH_WAIT_S"; then
        echo "FAILED: hexwrite did not finish $tpath (wanted a fresh HEXWRITE-OK after $n chunks)" >&2
        return 1
    fi
    if [ "$(wc -c < "$file" | tr -d ' ')" != "$size" ]; then
        echo "FAILED: $tpath changed size during upload" >&2
        return 1
    fi
    echo "ok:   uploaded $tpath ($size bytes in $n chunks; every chunk waited for its HEX-OK ack)"
}

# The dynamic refusal: a PT_INTERP ET_EXEC is step 2's material, not step 1's.
if ! upload /tmp/linux_run_dyn.elf "$DYN_ELF"; then
    report_and_exit
fi
if ! send_line "linux run /tmp/linux_run_dyn.elf"; then
    echo "FAILED: could not send the dynamic-refusal command (error='$SEND_ERR')" >&2
    report_and_exit
fi
if wait_marker "LINUX-RUN-ERR PT_INTERP requires a static ET_EXEC" "$ATTACH_WAIT_S"; then
    echo "ok:   a dynamic ET_EXEC is refused by name; no loader was entered without one"
else
    echo "FAILED: the dynamic ET_EXEC was not refused by name (no PT_INTERP refusal seen)" >&2
    report_and_exit
fi

# ── Phase 4: the MAX_IMAGE payload executes; the one-spawn limit then holds ─
if ! upload /tmp/linux_run_static.elf "$STATIC_ELF"; then
    report_and_exit
fi
if ! send_line "linux run /tmp/linux_run_static.elf"; then
    echo "FAILED: could not send the static spawn command (error='$SEND_ERR')" >&2
    report_and_exit
fi
if wait_marker "TENANT-EXEC-OK" "$ATTACH_WAIT_S"; then
    echo "ok:   the tenant's MAX_IMAGE static ELF executed and printed TENANT-EXEC-OK"
else
    echo "FAILED: the static ELF never executed (TENANT-EXEC-OK absent)" >&2
    report_and_exit
fi
if wait_marker "LINUX-RUN-OK checkpoint=none" "$ATTACH_WAIT_S"; then
    echo "ok:   linux run reports LINUX-RUN-OK checkpoint=none"
else
    echo "FAILED: linux run did not report LINUX-RUN-OK checkpoint=none" >&2
    report_and_exit
fi

# The first spawn succeeded. The one-kernel-spawn limit must now refuse a
# second request before any second child is created.
if ! send_line "linux run /tmp/linux_run_static.elf"; then
    echo "FAILED: could not send the second linux run (error='$SEND_ERR')" >&2
    report_and_exit
fi
if wait_marker "LINUX-RUN-ERR already-run" "$ATTACH_WAIT_S"; then
    echo "ok:   a second linux run is refused by the one-spawn limit"
else
    echo "FAILED: the second linux run was not refused as already-run" >&2
    report_and_exit
fi
if [ "$(count_marker 'TENANT-EXEC-OK')" != "1" ]; then
    echo "FAILED: the second linux run printed TENANT-EXEC-OK again (the one-spawn limit did not hold)" >&2
    report_and_exit
fi
if [ "$(count_marker 'LINUX-RUN-OK checkpoint=none')" != "1" ]; then
    echo "FAILED: the second linux run reported success again (the one-spawn limit did not hold)" >&2
    report_and_exit
fi
echo "ok:   the second request was refused; only one static child executed"

# ── Phase 5: no step 2 checkpoint slot, and no step 2 profile ─────────────
if grep -aqF "[ENV_CKPT] linux task 'linux.run.0'" "$LOG"; then
    echo "FAILED: the default tenant's record stamped linux.run.0 into the checkpoint boundary — step 1 must stay outside it" >&2
    report_and_exit
fi
echo "ok:   the kernel stamped no checkpoint slot for linux.run.0 (tasks 0/1 only)"

api "$W/profile.json" -X POST -d '{"index":5,"profile":"tenant-linux-ckpt"}' \
    "$BASE/api/partition/$pid/env" || true
if [ "$(jval "$W/profile.json" ok)" = "false" ] &&
   [ "$(jval "$W/profile.json" status)" = "5" ] &&
   grep -qF "unsupported" "$W/profile.json"; then
    echo "ok:   profile 1 (tenant-linux-ckpt) is refused as unsupported in this image"
else
    echo "FAILED: profile 1 was not refused as unsupported (ok='$(jval "$W/profile.json" ok)' status='$(jval "$W/profile.json" status)' error='$(jval "$W/profile.json" error)')" >&2
    report_and_exit
fi

# ── Phase 6: clean destroy ─────────────────────────────────────────────────
api "$W/destroy.json" -X POST -d "{\"env_id\":$E}" \
    "$BASE/api/partition/$pid/env/destroy" || true
if [ "$(jval "$W/destroy.json" ok)" = "true" ]; then
    echo "ok:   the tenant environment was destroyed over the E5 route"
else
    echo "FAILED: could not destroy the tenant environment (error='$(jval "$W/destroy.json" error)')" >&2
    report_and_exit
fi

if [ "$D" != "0" ] && [ -n "$D" ]; then
    echo "note: the console reported dropped=$D bytes during the run (the assertions above were made from the retained stream)" >&2
fi

echo "OK: an ordinary tenant uploaded a MAX_IMAGE-padded static ET_EXEC through hexwrite in acked 240-byte chunks and spawned it through linux_spawn; the child printed TENANT-EXEC-OK, a second linux run was refused already-run without a second child, a dynamic PT_INTERP image and a missing payload were refused by name, no checkpoint slot was claimed for linux.run.0, profile 1 stayed unsupported, and the environment was destroyed cleanly (step 1)"
exit 0

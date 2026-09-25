#!/usr/bin/env bash
# tests/env_create_boot_check.sh — proves POSIX-Environments E4 on the real
# target: on the E1 UNIFIED boot (grub menu entry 3 — the Ring-0 control plane
# and the Ring-3 sidecar world alive in ONE boot), the HTTP control plane can
# place a tenant POSIX environment INTO a partition created over HTTP, and the
# kernel's placement rules are enforced where it counts.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E4 is the partition-targeted-creation step of the POSIX-Environments roadmap
# (docs/AeroSLS-POSIX-Environments-Roadmap-v0.1.md §7). Its verification plan
# names exactly this check and was explicitly deferred "until E1 lands" (§7.1:
# "In the current Phase-5 self-hosted boot … there is no HTTP surface to drive
# env_service_create"), because the two halves it joins did not share a boot:
# the kernel's environment-manager RPC (kernel/env_service.c) could only be
# exercised from a boot in which the HTTP control plane and init are BOTH
# running, which is what E1 built.
#
# Everything E4 is made of is invisible from source alone: the kernel-held
# control channel (kernel.env.control) wired at boot, the round trip to init's
# Rust environment manager, the target_partition placement through
# cap_create_sidecar_in, the frames the child draws being charged to the
# TARGET partition rather than the creator's, and the kernel REFUSING a
# placement it must refuse. Each is one HTTP response or one serial line away
# from being a claim, so this check drives the API and reads the log.
#
# ─── What it asserts, and why each one needs a boot ────────────────────────
#   1. GET /api/health answers with a live body (the control plane is serving).
#   2. POST /api/partitions defines a partition and returns its id — the
#      surface E4's placement targets.
#   3. POST /api/partition/{id}/env creates an environment IN that partition.
#      This is the round trip that was unreportable until the wait loop learned
#      to yield to Ring 3: before that fix the kernel spun 6.25 M times,
#      received 0 replies, timed out after ~4.9 s and the API reported failure
#      for work init had actually done (see §7.2).
#   4. The environment's two sidecars (drv.ramdisk.<i>, aerosls.posix.<i>)
#      appear in GET /api/processes carrying partition_id == the target. This
#      is the roadmap's "its processes appear with that partition_id" — and it
#      is why /api/processes gained the field: nothing exposed a process's
#      partition, so the assertion had nothing to read.
#   5. GET /api/partitions shows the whole environment charged to the TARGET
#      partition and no part of it to the creator: its three regions (POSIX
#      heap 1024 + ramdisk heap 64 + storage 256 = 1344 frames, the bulk of an
#      environment) through init's alloc_region_in — the E4 follow-on whose
#      absence this check first measured at +1360 on PARTITION_SYSTEM and
#      +176 on the tenant (§7.2) — and its two sidecars' image/stack/syscall-
#      stack frames through cap_create_sidecar_in's
#      allocate_physical_ram_frame_for_partition(child_partition).
#   6. A placement the kernel must refuse IS refused: create a second
#      partition, PAUSE it, and create an environment in it. The kernel's own
#      gate answers (CAP_EPERM "target partition N is paused — cannot charge
#      it"), the API answers ok:false — and NOTHING is allocated for it: the
#      gate now fires on the environment's first region request, so the 1344
#      frames a refused placement used to leak to the creator are never taken
#      (asserted). Nothing is placed in the refused target either
#      (frame_usage 0, no process carrying that partition). The same is
#      asserted for a target that does not exist (CAP_EINVAL).
#
# Assertion 6 is the one a boot is the only place to make: whether a placement
# is refused, and whether the refusal is real (nothing lands anyway), is not
# visible in a unit test of the caller.
#
# Prerequisite: sls_operating_system.iso (make x86-iso; the unified entry
# needs sidecars.cpio present — commit it, so CI ships it) and
# qemu-system-x86_64. Reads nothing else; touches only its temp log.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Teeth: tests/env_create_boot_check_smoke.sh — it points this guard at two
# inputs that must turn it red (the kernel-only boot, which has no env manager
# at all; and E4_TOOTH=skip-pause, which withholds the pause that the refusal
# assertion depends on, so a guard whose refusal arm was vacuous would pass
# there and the smoke would catch it) and then requires it to pass on the real
# boot. The matching SOURCE control — deleting the paused check in
# cap_create_sidecar_in — was run by hand and turns this guard red naming the
# refusal arm (recorded in §7.2).
#
# Env knobs (used by the smoke; the defaults are what every other caller gets):
#   E4_ISO          path to the ISO to boot        (default sls_operating_system.iso)
#   E4_BOOT_ENTRY   1-based grub MENU POSITION     (default 3 = the unified entry)
#   E4_WINDOW_S     seconds to wait for the markers (default 120)
#   E4_TOOTH        smokes only: `skip-pause` withholds the pause before the
#                   refused-placement arm, so its assertion MUST fail
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first),
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E4_ISO:-sls_operating_system.iso}"
LOG=/tmp/aerosls_env_create_boot.log
ENTRY="${E4_BOOT_ENTRY:-3}"     # 1 = Phase-5 initrd boot, 2 = kernel-only,
                                # 3 = unified (the 1-based menu position
                                # tests/grub_select_kernel_only.sh boots by
                                # sending ENTRY-1 Down keys)
WINDOW_S="${E4_WINDOW_S:-120}"
TOOTH="${E4_TOOTH:-}"

# The API's own token/role model: DB_ADMIN, which is what partition create,
# partition pause and an environment placement all require (tools/aeroslsctl's
# default token — the same one the other HTTP-driven boot checks use).
TOKEN=deadbeef01234567cafebabe76543210

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

SER=/tmp/aerosls_env_create_ser
rm -f "$SER.in" "$SER.out" "$LOG"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

# Host port: the first free loopback one, never a fixed number (the deploy gate
# runs sibling checks while live cluster nodes hold 3000+i; tests/free_port.sh
# is the repo's shared allocator).
PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
BASE="http://127.0.0.1:$PORT"
W="$(mktemp -d)"
qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

# No disk is attached, deliberately — the same choice tests/unified_boot_check.sh
# makes and for the same reason: this check asserts nothing about storage, and a
# private fresh image would add the kernel's NVMe bring-up and TLS-CA
# persistence path to a boot whose subject is partition placement.
# Accelerator: KVM when the host exposes it, else the documented multi-threaded
# TCG fallback (same detection as the other boot checks).
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
    -device e1000,netdev=net0,mac=52:54:00:12:34:03 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>"$W/qemu.err" &
QPID=$!

cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# Select the unified entry (grub's serial menu; see the helper's header).
bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
    qemu_err
    exit 1
}

hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }

# ── Phase 1: wait until the boot is far enough along to assert on ───────────
# Bounded by WINDOW_S and by QEMU's liveness. The two contradiction branches
# below are the guard's honesty checks (same shape as unified_boot_check.sh): a
# boot that is not the unified boot is this guard pointed at the wrong thing,
# and they name that as soon as the kernel makes it visible instead of waiting
# out the window for the same verdict.
ready=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    if grep -aq "\[BOOT\] command line:" "$LOG" 2>/dev/null &&
       ! grep -aq "unified=1" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY booted without unified=1 on the kernel command line (this is not the unified boot)" >&2
        grep -a "\[BOOT\] command line:" "$LOG" | head -1 | sed 's/^/      /' >&2
        exit 1
    fi
    if grep -aq "Listening on port 3000" "$LOG" 2>/dev/null &&
       ! grep -aq "\[E1\] control plane planted" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY reached its HTTP listener without planting the Ring-0 control plane — the environment manager cannot be driven from this boot" >&2
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
    echo "FAIL  the E4 environment-create path (partition -> env -> partition-tagged processes)" >&2
    echo "      E1/INIT/ENV/SIDECAR lines seen:" >&2
    grep -a "\[E1\]\|\[INIT\]\|\[ENV\]\|\[SIDECAR\]\|\[CS\]\|\[PARTITION\]" "$LOG" 2>/dev/null | head -30 | sed 's/^/      /' >&2
    echo "      last 20 serial lines:" >&2
    tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    exit 1
}

if [ "$ready" -ne 1 ]; then
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAILED: QEMU exited before the unified boot reached its markers — the boot crashed" >&2
        qemu_err
    else
        echo "FAILED: markers missing within ${WINDOW_S}s (heartbeats: $(hb_now), yields: $(grep -ac '\[E1\] yield' "$LOG" 2>/dev/null))" >&2
    fi
    report_and_exit
fi
echo "ok:   the unified boot is up (control plane serving, init making progress)"

# ── HTTP helpers ───────────────────────────────────────────────────────────
# Every request retries: one refused connection is a host-side hiccup (the
# guest's accept path is Ring-0 work on a CPU shared with Ring 3), while a
# control plane that is genuinely not serving fails every attempt.
api() {   # api <out-file> <curl args...>
    local out="$1"; shift
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
jval() {   # jval <json-file> <key> — the value, or empty when absent/not JSON
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
proc_pid() {   # proc_pid <processes.json> <name> <partition> — its pid, or empty
    # Partition-scoped on purpose: sidecar names repeat across partitions by
    # design (E2), and this boot HAS the collision — init's own drv.ramdisk.0
    # lives in PARTITION_SYSTEM while the environment's lives in the target —
    # so matching on the name alone would read the wrong process.
    python3 - "$1" "$2" "$3" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
for p in d.get("processes", []):
    if p.get("name") == sys.argv[2] and str(p.get("partition_id")) == str(sys.argv[3]):
        print(p.get("pid", "")); break
else:
    print("")
PY
}
frame_usage() {   # frame_usage <partitions.json> <id> — frame_usage, or empty
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
for p in d.get("partitions", []):
    if str(p.get("id")) == str(sys.argv[2]):
        print(p.get("frame_usage", "")); break
else:
    print("")
PY
}
proc_count_for_partition() {   # proc_count_for_partition <processes.json> <id>
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
print(sum(1 for p in d.get("processes", []) if str(p.get("partition_id")) == str(sys.argv[2])))
PY
}
partitions_snapshot() { api "$W/parts.json" "$BASE/api/partitions" || true; }
processes_snapshot()  { api "$W/procs.json" "$BASE/api/processes"  || true; }

# ── Phase 2: the control plane answers with a live body ────────────────────
api "$W/health.json" "$BASE/api/health" || true
if [ "$(jval "$W/health.json" status)" = "ok" ]; then
    echo "ok:   /api/health answers with a live kernel body"
else
    echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
    fail=1
fi

# ── Phase 3: a partition over HTTP — the placement target ──────────────────
pname="e4boot"
api "$W/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || true
id="$(jval "$W/pcreate.json" partition_id)"
ok="$(jval "$W/pcreate.json" ok)"
if [ "$ok" = "true" ] && [ -n "$id" ] && [ "$id" != "0" ] && [ "$id" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined '$pname' as partition $id"
else
    echo "FAILED: POST /api/partitions did not define a partition (ok='$ok' id='$id')" >&2
    jval "$W/pcreate.json" error >&2 || true
    fail=1
fi
# The kernel's own statement of the same fact (kernel TX, so it is written
# whole even though the unified boot has two console writers).
if grep -aqF "[PARTITION] created '$pname' (id=$id" "$LOG"; then
    echo "ok:   the kernel logged the partition it created"
else
    echo "FAILED: no '[PARTITION] created' line for '$pname' (id=$id) in the serial log" >&2
    fail=1
fi

# ── Phase 4: the environment create round trip ─────────────────────────────
partitions_snapshot
sys_usage_before="$(frame_usage "$W/parts.json" 0)"
target_before="$(frame_usage "$W/parts.json" "$id")"
[ -n "$target_before" ] || target_before=0

t0=$(date +%s%N 2>/dev/null || echo 0)
api "$W/env.json" -X POST -d '{"index":0}' "$BASE/api/partition/$id/env" || true
t1=$(date +%s%N 2>/dev/null || echo 0)
env_ok="$(jval "$W/env.json" ok)"
env_id="$(jval "$W/env.json" env_id)"
env_part="$(jval "$W/env.json" partition)"
if [ "$env_ok" = "true" ] && [ -n "$env_id" ] && [ "$env_id" != "0" ] && [ "$env_part" = "$id" ]; then
    if [ "$t0" != "0" ]; then
        echo "ok:   POST /api/partition/$id/env created env $env_id in partition $env_part ($(( (t1 - t0) / 1000000 )) ms)"
    else
        echo "ok:   POST /api/partition/$id/env created env $env_id in partition $env_part"
    fi
else
    echo "FAILED: the environment manager did not create an environment in partition $id (ok='$env_ok' env_id='$env_id' partition='$env_part')" >&2
    jval "$W/env.json" error >&2 || true
    grep -a "\[ENV\]\|\[CS\]\|\[SIDECAR\]" "$LOG" | tail -10 | sed 's/^/      /' >&2
    fail=1
fi
# The kernel created the environment's two sidecars — an environment is a
# ramdisk driver plus a POSIX sidecar (roadmap §11), and both are placed in the
# target by the same call.
for nm in "drv.ramdisk.0" "aerosls.posix.0"; do
    if grep -aqF "'$nm'" "$LOG"; then
        echo "ok:   the kernel created the environment's sidecar '$nm'"
    else
        echo "FAILED: no '[SIDECAR] …' line for '$nm' — the environment was not built" >&2
        fail=1
    fi
done

# ── Phase 5: the processes carry that partition ────────────────────────────
# The roadmap's own assertion (§7.1): "assert its processes appear with that
# partition_id". Read from /api/processes, which now carries the field.
processes_snapshot
rd_pid=""; px_pid=""
for nm in "drv.ramdisk.0" "aerosls.posix.0"; do
    p="$(proc_pid "$W/procs.json" "$nm" "$id")"
    if [ -n "$p" ]; then
        echo "ok:   '$nm' runs in partition $id as PID $p (the target, not the creator's)"
        case "$nm" in
            drv.ramdisk.0)   rd_pid="$p" ;;
            aerosls.posix.0) px_pid="$p" ;;
        esac
    else
        echo "FAILED: no process named '$nm' reports partition_id=$id (see $W/procs.json)" >&2
        fail=1
    fi
done
# The environment's POSIX sidecar must be wired to the environment's OWN
# ramdisk, not to the system's identically-named one — partition-scoped peer
# resolution (E2) is what makes the repeat possible, and the wire line names
# both pids, so the pairing is checkable rather than assumed (roadmap G5).
rd_sys_pid="$(proc_pid "$W/procs.json" "drv.ramdisk.0" 0)"
if [ -n "$rd_sys_pid" ]; then
    echo "note: the boot's own 'drv.ramdisk.0' (PID $rd_sys_pid, partition 0) coexists with the environment's (partition $id) — names are partition-scoped (E2)"
fi
if [ -n "$px_pid" ] && [ -n "$rd_pid" ]; then
    if grep -aqF "PID $px_pid 'aerosls.posix.0': wired chan 'ramdisk' to sidecar 'drv.ramdisk.0' (PID $rd_pid" "$LOG"; then
        echo "ok:   the environment's POSIX sidecar is wired to its own ramdisk (PID $rd_pid), not the system's"
    else
        echo "FAILED: no wire line pairing PID $px_pid ('aerosls.posix.0') with PID $rd_pid (the environment's 'drv.ramdisk.0')" >&2
        grep -a "wired chan 'ramdisk'" "$LOG" | tail -4 | sed 's/^/      /' >&2
        fail=1
    fi
fi
sys_procs="$(proc_count_for_partition "$W/procs.json" 0)"
if [ -z "$sys_procs" ] || [ "$sys_procs" = "0" ]; then
    echo "FAILED: no process reports partition 0 — /api/processes could not be read, or the boot tree is unaccounted for" >&2
    fail=1
fi

# ── Phase 6: the environment's frames are charged to the target ────────────
# The roadmap's other half of §7's plan, and the E4 follow-on: ALL of an
# environment's memory lands on its partition. Its two sidecars' image/stack/
# syscall-stack frames are charged by cap_create_sidecar_in
# (allocate_physical_ram_frame_for_partition(child_partition)), and its three
# regions — POSIX_HEAP 1024 + RD_HEAP 64 + RD_STORAGE 256 = 1344 frames, the
# bulk of an environment — by init's alloc_region_in. Before that variant
# existed, init allocated them with the caller-charged alloc_region, so they
# were billed to PARTITION_SYSTEM: measured then, the creator moved by +1360
# and this partition by only +176 (roadmap §7.2). The floor is the regions
# alone; the sidecar frames are the margin above it.
partitions_snapshot
target_after="$(frame_usage "$W/parts.json" "$id")"
sys_usage_after="$(frame_usage "$W/parts.json" 0)"
[ -n "$target_after" ] || target_after=0
delta_target=$(( target_after - target_before ))
delta_sys=$(( ${sys_usage_after:-0} - ${sys_usage_before:-0} ))
if [ "$delta_target" -ge 1344 ] 2>/dev/null; then
    echo "ok:   the target partition carries the whole environment (+$delta_target frames: its 1344 region frames and its sidecars, now $target_after)"
else
    echo "FAILED: partition $id's frame_usage grew by only $delta_target frames across the create ($target_before -> $target_after) — the environment's heap/storage regions are not charged to it" >&2
    fail=1
fi
if [ "$delta_sys" -le 64 ] 2>/dev/null; then
    echo "ok:   the creator (PARTITION_SYSTEM) was not charged for the tenant's memory (+$delta_sys frames)"
else
    echo "FAILED: the creator's frame_usage grew by $delta_sys frames across a tenant environment's create — the environment's regions are still billed to init, not to the tenant" >&2
    fail=1
fi

# ── Phase 7: a rejected placement places nothing ───────────────────────────
# The kernel's target gate must REFUSE a target that cannot be populated, and
# the refusal must be real: nothing charged to the target, no process in it.
# Two arms — a paused target (CAP_EPERM) and an absent one (CAP_EINVAL). The
# gate that answers now is sys_sls_alloc_region's: the env manager reaches it
# FIRST, before it asks for either sidecar, which is what makes a refusal cost
# nothing (below). cap_create_sidecar_in applies the same three rules to the
# child, but by then the regions are already allocated.
pname2="e4bootpaused"
api "$W/pcreate2.json" -X POST -d "{\"name\":\"$pname2\"}" "$BASE/api/partitions" || true
id2="$(jval "$W/pcreate2.json" partition_id)"
if [ "$(jval "$W/pcreate2.json" ok)" = "true" ] && [ -n "$id2" ]; then
    echo "ok:   POST /api/partitions defined '$pname2' as partition $id2"
else
    echo "FAILED: could not define the second partition (id='$id2')" >&2
    fail=1
fi

if [ "$TOOTH" = "skip-pause" ]; then
    # The tooth's input: the pause is withheld, so a targeted create into this
    # partition is ALLOWED — exactly what removing the kernel's own check would
    # do — and the refusal assertions below must go red.
    echo "note: E4_TOOTH=skip-pause — not pausing partition $id2; the refusal assertions below MUST fail"
else
    api "$W/pause.json" -X POST -d "{\"partition_id\":$id2}" "$BASE/api/partition/pause" || true
    if [ "$(jval "$W/pause.json" ok)" = "true" ]; then
        echo "ok:   POST /api/partition/pause paused partition $id2"
    else
        echo "FAILED: could not pause partition $id2 (ok='$(jval "$W/pause.json" ok)')" >&2
        fail=1
    fi
fi

api "$W/env2.json" -X POST -d '{"index":0}' "$BASE/api/partition/$id2/env" || true
if [ "$(jval "$W/env2.json" ok)" = "false" ]; then
    echo "ok:   POST /api/partition/$id2/env was refused ($(jval "$W/env2.json" error))"
else
    echo "FAILED: a create into partition $id2 was NOT refused (ok='$(jval "$W/env2.json" ok)' env_id='$(jval "$W/env2.json" env_id)') — the kernel's placement gate did not fire" >&2
    fail=1
fi
if [ "$TOOTH" = "skip-pause" ]; then
    :   # no kernel-side refusal line to look for on the tooth's input
elif grep -aqE "\[ALLOC_REGION\] CAP_EPERM: target partition $id2 is paused" "$LOG"; then
    # The refusal now comes from the FIRST thing the env manager does for a
    # nonzero target — its region allocation — which is why nothing at all is
    # allocated: the gate fires before any frame is taken (the E4 follow-on
    # removed the refused-placement leak below instead of just re-billing it).
    echo "ok:   the kernel refused it in its own words (CAP_EPERM: target partition $id2 is paused — cannot charge it)"
else
    echo "FAILED: the refusal did not come from the kernel's paused-target gate (no '[ALLOC_REGION] CAP_EPERM: target partition $id2 is paused' line)" >&2
    grep -a "\[ALLOC_REGION\]\|\[CS\]" "$LOG" | tail -5 | sed 's/^/      /' >&2
    fail=1
fi
# The refusal must be real: nothing charged to the refused target, nothing in it.
partitions_snapshot
refused_usage="$(frame_usage "$W/parts.json" "$id2")"
[ -n "$refused_usage" ] || refused_usage=0
# A refused placement must allocate NOTHING, anywhere. This is the other half
# of the E4 follow-on: while the regions were allocated with the
# caller-charged alloc_region, init took them BEFORE asking the kernel to
# place the sidecars, so a refusal left 1344 frames charged to the creator
# with nothing holding them (measured: exactly +1344 per refused create).
# Charging the target through the same gate that refuses the placement means
# the refusal now happens before any frame is taken.
refused_sys="$(frame_usage "$W/parts.json" 0)"
delta_sys_refused=$(( ${refused_sys:-0} - ${sys_usage_after:-0} ))
if [ "$TOOTH" = "skip-pause" ]; then
    :   # on the tooth's input the placement succeeded, so this is not asserted
elif [ "$delta_sys_refused" -le 64 ] 2>/dev/null; then
    echo "ok:   the refused placement allocated nothing (the creator moved by $delta_sys_refused frames)"
else
    echo "FAILED: the refused placement took $delta_sys_refused frames from the creator with nothing holding them" >&2
    fail=1
fi
processes_snapshot
refused_procs="$(proc_count_for_partition "$W/procs.json" "$id2")"
if [ "$refused_usage" = "0" ] && [ "$refused_procs" = "0" ]; then
    echo "ok:   nothing was placed in the refused partition ($refused_usage frames, $refused_procs processes)"
else
    echo "FAILED: the refused partition was populated anyway ($refused_usage frames, $refused_procs processes)" >&2
    fail=1
fi

# The absent-target arm: a target that is not a partition at all (0xFFFFFFFF
# cannot exist — PARTITION_MAX partitions, slot 0 the system's). This is the
# other branch of the same gate, and it needs no pause.
api "$W/env3.json" -X POST -d '{"index":0}' "$BASE/api/partition/4294967295/env" || true
if [ "$(jval "$W/env3.json" ok)" = "false" ]; then
    echo "ok:   POST /api/partition/4294967295/env was refused ($(jval "$W/env3.json" error))"
else
    echo "FAILED: a create into a nonexistent partition was not refused (ok='$(jval "$W/env3.json" ok)')" >&2
    fail=1
fi
if [ "$TOOTH" = "skip-pause" ]; then
    :
elif grep -aqF "[ALLOC_REGION] CAP_EINVAL: target partition 4294967295 is not an active partition" "$LOG"; then
    echo "ok:   the kernel refused the absent target in its own words (CAP_EINVAL)"
else
    echo "FAILED: no '[ALLOC_REGION] CAP_EINVAL: target partition … is not an active partition' line for the absent target" >&2
    fail=1
fi

# ── Stop the guest (bounded: SIGTERM, 10 s grace, SIGKILL) ─────────────────
kill "$QPID" 2>/dev/null || true
for _i in $(seq 1 10); do
    kill -0 "$QPID" 2>/dev/null || break
    case "$(ps -o stat= -p "$QPID" 2>/dev/null)" in Z*|'') break ;; esac
    sleep 1
done
kill -9 "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$CATPID" 2>/dev/null || true

[ "$fail" -eq 0 ] || report_and_exit

echo "OK: the unified boot created a POSIX environment in an HTTP-created partition — carrying it, charged to it, and refused where the kernel refuses (E4)"
exit 0

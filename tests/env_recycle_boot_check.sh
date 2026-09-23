#!/usr/bin/env bash
# tests/env_recycle_boot_check.sh — proves POSIX-Environments E5 on the real
# target: the unified boot's HTTP control plane can create and DESTROY
# environments in a loop longer than every fixed-size table an environment
# occupies, and every one of those resources comes back.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E5 is the lifecycle phase of the POSIX-Environments roadmap
# (docs/AeroSLS-POSIX-Environments-Roadmap-v0.1.md §8), and its verification
# plan is explicit: "A recycle program in the style of part_recycle.c: N cycles
# of partition create -> environment create -> run a command -> destroy, where
# N exceeds PROC_MAX and SIDECAR_REGISTRY_MAX. Assert process slots, registry
# entries and the system frame count are stable across all N. Tooth: skip the
# registry cleanup — the cycle that exceeds the registry size fails with
# registry full."
#
# This is that check. "Run a command" is the one clause it cannot honour yet,
# and the reason is structural rather than an omission: reaching a command
# inside an environment needs E6's terminals (an environment's console is
# `kernel.debug.console` today, not a per-environment channel pair, and there
# is no attach surface at all). What a cycle CAN prove without one is that the
# environment really came up on its own storage and was placed in the target
# — its two sidecars (drv.ramdisk.<i>, aerosls.posix.<i>) are live and carry
# the target partition_id after every create — and that is asserted on every
# cycle. The placement, wiring and per-partition charging of those sidecars are
# E4's `env_create_boot_check.sh`; this guard's subject is that they can be
# taken away again, N times, without a resource failing to return.
#
# ─── What it asserts, and why each one needs a boot ────────────────────────
#   1. GET /api/health answers with a live body (the control plane is serving).
#   2. A partition exists to place environments in (POST /api/partitions).
#   3. N cycles of POST /api/partition/{id}/env {index:i} followed by
#      POST /api/partition/{id}/env/destroy {env_id} all succeed, with the
#      environment's two sidecars live in the target partition after each
#      create. N exceeds PROC_MAX (16) and SIDECAR_REGISTRY_MAX (16), and the
#      environment table holds ONE environment at a time, so every cycle after
#      the first succeeds ONLY if the previous destroy really released the
#      slots it took: the environment table (MAX_ENVIRONMENTS 8 in init), the
#      sidecar registry (SIDECAR_REGISTRY_MAX 16), the process table (PROC_MAX
#      16, two slots per environment) and the environment id counter. The
#      tooth input measures those ceilings from the other side: with every
#      destroy withheld the loop dies on whichever runs out first, and the
#      guard must name that cycle. Measured on the tooth input — the process
#      table is smallest, because the boot's own five processes share it: the
#      sixth environment's SECOND sidecar cannot get a slot (5 + 12 = 17 > 16,
#      which is why the log shows 'drv.ramdisk.6' created and the POSIX create
#      failing after it, and why the API answers ENV_ERR_PART: the kernel's
#      process allocator returns an error with no line of its own).
#   4. The NAME is reusable after a destroy. The final cycle creates index 1
#      again in the same partition — the name cycle 1 registered,
#      `drv.ramdisk.1`/`aerosls.posix.1`. The kernel refuses a duplicate
#      registration inside one partition (E2's scoping keys on name+partition),
#      so this is the roadmap's tooth made observable WITHOUT sabotaging a
#      source: the registry entry has to be gone for the name to be creatable
#      again.
#   5. The FRAME COUNT is stable. PARTITION_SYSTEM's frame_usage is taken before
#      the loop and after it: an environment draws 1344 region frames plus two
#      sidecars' image/stack frames, all charged to its own partition, and a
#      destroy that forgot one of them shows up as a drift far above noise.
#      The tenant partition's own usage returns too.
#   6. Nothing survives in the process tree: after the loop no process in the
#      target partition is one of the loop's sidecar names.
#   7. An environment SURVIVES a partition pause and is still destroyable after
#      the resume (§8's pause/resume bullet): pause the partition, assert a new
#      placement is refused AND that the existing environment was not killed by
#      the pause (a paused partition stops scheduling its processes; it does not
#      end them), resume, then destroy it and assert its frames come back. The
#      half this cannot reach is stated in the residual note below.
#   8. A PARTITION DESTROY IS AN ENVIRONMENT ENDING (§8's other bullet). Create
#      a second partition and an environment in it, destroy the PARTITION, and
#      then ask to destroy that environment: the manager answers
#      ENV_ERR_NOENT ("no such environment"), not ENV_OK. That difference is
#      the whole assertion. Without it init would still be holding an
#      environment whose sidecars and frames a partition teardown already
#      reclaimed, and its destroy would issue a region release against a
#      partition that no longer exists. The kernel's own refusal of that
#      release is in the serial log (`[FREE_REGION] CAP_EINVAL: target
#      partition N is not an active partition`) and is asserted too, because it
#      is kernel TX and therefore cannot be lost to a console race.
#
# ─── Residual: what this guard does NOT prove ──────────────────────────────
# "Confirm environments stop and resume cleanly" (§8) is asserted at the
# partition level: while paused, a placement is refused and the existing
# environment is not killed; after resume the partition is serviceable and that
# environment is still destroyable with its accounting intact. What is NOT
# observable today is that the environment's own execution stopped while paused
# — an environment's output has no per-environment channel to read until E6
# gives it one (its console is still the shared kernel console), and a paused
# partition's processes are parked in exactly the state a running one's are.
# The scheduler-level claim is unchanged and already tested elsewhere; this is
# a statement about what THIS guard can see, not a claim that pause is unproven.
#
# Prerequisite: sls_operating_system.iso (make x86-iso; the unified entry needs
# sidecars.cpio present — commit it, so CI ships it) and qemu-system-x86_64.
# Reads nothing else; touches only its temp log.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Teeth: tests/env_recycle_boot_check_smoke.sh — it points this guard at inputs
# that do NOT have the E5 property (the kernel-only boot, which has no
# environment manager at all; and E5_TOOTH=skip-destroy, which withholds the
# destroy so the loop cannot recycle, which is the roadmap's own tooth
# reproduced without a second kernel build) and requires this guard to go red
# on each, then requires it to pass on the real boot. A guard whose stability
# assertions were vacuous — counting creates it never checked, or tolerating a
# drift as large as an environment — passes the tooth inputs, and the smoke
# fails it for that.
#
# Env knobs (used by the smoke; the defaults are what every other caller gets):
#   E5_ISO          path to the ISO to boot             (default sls_operating_system.iso)
#   E5_BOOT_ENTRY   1-based grub MENU POSITION          (default 3 = the unified entry)
#   E5_WINDOW_S     seconds to wait for the boot markers (default 180)
#   E5_CYCLES       create->destroy cycles              (default 18; must exceed
#                   PROC_MAX and SIDECAR_REGISTRY_MAX, both 16)
#   E5_TOOTH        smokes only: `skip-destroy` withholds every destroy, so the
#                   loop cannot recycle and its stability assertions MUST fail
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first),
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E5_ISO:-sls_operating_system.iso}"
LOG=/tmp/aerosls_env_recycle_boot.log
ENTRY="${E5_BOOT_ENTRY:-3}"     # 1 = Phase-5 initrd boot, 2 = kernel-only,
                                # 3 = unified (the 1-based menu position
                                # tests/grub_select_kernel_only.sh boots by
                                # sending ENTRY-1 Down keys)
WINDOW_S="${E5_WINDOW_S:-180}"
CYCLES="${E5_CYCLES:-18}"
TOOTH="${E5_TOOTH:-}"

# The API's own token/role model: DB_ADMIN, which is what partition create,
# pause and an environment's create AND destroy all require
# (tools/aeroslsctl's default token — the same one the other HTTP-driven boot
# checks use).
TOKEN=deadbeef01234567cafebabe76543210

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

SER=/tmp/aerosls_env_recycle_ser
rm -f "$SER.in" "$SER.out" "$LOG"
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

# Host port: the first free loopback one, never a fixed number (the deploy gate
# runs sibling checks while live cluster nodes hold 3000+i; tests/free_port.sh
# is the repo's shared allocator).
PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free port in 3001..3020" >&2; exit 2; }
BASE="http://127.0.0.1:$PORT"
W="$(mktemp -d)"
qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

# No disk is attached, deliberately — the same choice tests/unified_boot_check.sh
# and env_create_boot_check.sh make and for the same reason: this check asserts
# nothing about storage, and a private fresh image would add the kernel's NVMe
# bring-up and TLS-CA persistence path to a boot whose subject is lifecycle.
ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
        ACCEL="-accel kvm"
    else
        ACCEL="-accel tcg,thread=multi"
    fi
fi

SMP="${E5_SMP:-4}"   # knob for the single-core control: a loop over the
                     # kernel's shared cap structures is exactly where a
                     # two-core race would show (see the header's control
                     # record), so the guard must be able to run one core.
qemu-system-x86_64 -cdrom "$ISO" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:05 \
    -display none -m 4G -smp "$SMP" -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>"$W/qemu.err" &
QPID=$!

cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
    echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
    qemu_err
    exit 1
}

hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }

# ── Phase 1: wait until the boot is far enough along to assert on ───────────
# Bounded by WINDOW_S and by QEMU's liveness. The two contradiction branches
# are the guard's honesty checks (same shape as unified_boot_check.sh): a boot
# that is not the unified boot is this guard pointed at the wrong thing, and
# they name that as soon as the kernel makes it visible instead of waiting out
# the window for the same verdict.
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
    echo "FAIL  the E5 environment recycle path (create -> destroy x$CYCLES stable)" >&2
    echo "      E1/INIT/ENV/SIDECAR/FREE_REGION/PARTITION lines seen:" >&2
    grep -a "\[E1\]\|\[INIT\]\|\[ENV\]\|\[SIDECAR\]\|\[FREE_REGION\]\|\[PARTITION\]" "$LOG" 2>/dev/null | tail -40 | sed 's/^/      /' >&2
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
    # The out file is CLEARED first, and that is not tidiness: callers read
    # their verdict out of it, so without this a request that never landed
    # (guest wedged, connection refused, timeout) leaves the PREVIOUS
    # request's body in place and the caller reads its own stale success.
    # Measured: the recycle loop's create at cycle 11 reported `ok=true` with
    # an env_id while the kernel's own env service log showed no create request
    # after the previous cycle's destroy — the loop had wedged that cycle
    # before, and the guard was reading the response to a request that had
    # already failed. An empty file makes every caller's `ok` empty, which is
    # not `true`, so a dead control plane can only ever read as failure.
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
proc_count() {   # proc_count <processes.json> <partition>
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
print(sum(1 for p in d.get("processes", []) if str(p.get("partition_id")) == str(sys.argv[2])))
PY
}
has_env_sidecars() {   # has_env_sidecars <processes.json> <partition> <index>
    # PRESENCE, not equality: the guard's own loop holds one environment at a
    # time, but the tooth input does not, and an assertion that only holds
    # while nothing accumulates would make the tooth fail for the wrong reason
    # (it did, on this smoke's first run: the guard went red at cycle 2 because
    # it saw two environments' sidecars rather than because nothing recycled).
    # Conservation is asserted where it belongs — on the end state, below.
    python3 - "$1" "$2" "$3" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(1)
want = {"aerosls.posix." + sys.argv[3], "drv.ramdisk." + sys.argv[3]}
got = {p.get("name", "") for p in d.get("processes", [])
       if str(p.get("partition_id")) == str(sys.argv[2])}
sys.exit(0 if want <= got else 1)
PY
}
part_env_names() {   # part_env_names <processes.json> <partition> — env sidecar names
    # Partition-scoped on purpose: sidecar names repeat across partitions by
    # design (E2), and this boot HAS the collision — init's own drv.ramdisk.N
    # lives in PARTITION_SYSTEM — so matching on the name alone would read the
    # wrong process.
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
names = sorted(p.get("name", "") for p in d.get("processes", [])
               if str(p.get("partition_id")) == str(sys.argv[2])
               and (str(p.get("name","")).startswith("aerosls.posix.")
                    or str(p.get("name","")).startswith("drv.ramdisk.")))
print(" ".join(names))
PY
}
partitions_snapshot() { api "$W/parts.json" "$BASE/api/partitions" || true; }
processes_snapshot()  { api "$W/procs.json" "$BASE/api/processes"  || true; }

# env_create <partition> <index> — creates, echoes "<env_id> <ok>"; also
# records the environment's sidecars as seen by /api/processes in
# $W/cycle.json when it asked for them (empty string when the request failed).
env_create() {
    local part="$1" idx="$2"
    api "$W/env.json" -X POST -d "{\"index\":$idx}" "$BASE/api/partition/$part/env" || true
    local ok env_id
    ok="$(jval "$W/env.json" ok)"
    env_id="$(jval "$W/env.json" env_id)"
    printf '%s %s\n' "${env_id:-0}" "${ok:-}"
}
env_destroy() {   # env_destroy <partition> <env_id> — echoes "<ok> <error>"
    local part="$1" id="$2"
    api "$W/destroy.json" -X POST -d "{\"env_id\":$id}" \
        "$BASE/api/partition/$part/env/destroy" || true
    printf '%s %s\n' "$(jval "$W/destroy.json" ok)" "$(jval "$W/destroy.json" error)"
}

# ── Phase 2: the control plane answers with a live body ────────────────────
api "$W/health.json" "$BASE/api/health" || true
if [ "$(jval "$W/health.json" status)" = "ok" ]; then
    echo "ok:   /api/health answers with a live kernel body"
else
    echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
    fail=1
fi

# ── Phase 3: a partition over HTTP — the placement target ──────────────────
pname="e5recycle"
api "$W/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || true
pid="$(jval "$W/pcreate.json" partition_id)"
if [ "$(jval "$W/pcreate.json" ok)" = "true" ] && [ -n "$pid" ] && [ "$pid" != "0" ] && [ "$pid" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined '$pname' as partition $pid"
else
    echo "FAILED: POST /api/partitions did not define a partition (id='$pid')" >&2
    jval "$W/pcreate.json" error >&2 || true
    report_and_exit
fi

partitions_snapshot; processes_snapshot
sys_before="$(frame_usage "$W/parts.json" 0)"
part_before="$(frame_usage "$W/parts.json" "$pid")"
sys_procs_before="$(proc_count "$W/procs.json" 0)"
[ -n "$sys_before" ] || sys_before=0
[ -n "$part_before" ] || part_before=0

# ── Phase 4: the recycle loop ──────────────────────────────────────────────
# N cycles, N > PROC_MAX (16) and > SIDECAR_REGISTRY_MAX (16). One environment
# at a time: cycle i creates index i, asserts the environment is REALLY up (its
# two sidecars live in the target partition), then destroys it. Every cycle
# after the first therefore depends on the previous destroy having released
# every slot it took.
echo "note: $CYCLES cycles (> PROC_MAX and SIDECAR_REGISTRY_MAX, both 16), one environment live at a time"
if [ "$TOOTH" = "skip-destroy" ]; then
    echo "note: E5_TOOTH=skip-destroy — withhold each destroy, so the loop CANNOT recycle (this run must fail)"
fi
cycle_fail=""
i=1
while [ "$i" -le "$CYCLES" ]; do
    read -r env_id ok <<EOF
$(env_create "$pid" "$i")
EOF
    if [ ! -s "$W/env.json" ]; then
        cycle_fail="cycle $i: create request got no reply at all (the control plane stopped answering — see the serial log; this is NOT a create that was refused)"
        break
    fi
    if [ "$ok" != "true" ] || [ -z "$env_id" ] || [ "$env_id" = "0" ]; then
        cycle_fail="cycle $i: create failed (env_id='$env_id' ok='$ok' error='$(jval "$W/env.json" error)')"
        break
    fi
    # The environment is really running: both sidecars visible, in the target.
    processes_snapshot
    if ! has_env_sidecars "$W/procs.json" "$pid" "$i"; then
        cycle_fail="cycle $i: the environment's sidecars are not both live in partition $pid (saw '$(part_env_names "$W/procs.json" "$pid")', wanted index $i's pair among them)"
        break
    fi
    if [ "$TOOTH" = "skip-destroy" ]; then
        i=$((i + 1))
        continue
    fi
    read -r dok derr <<EOF
$(env_destroy "$pid" "$env_id")
EOF
    if [ "$dok" != "true" ]; then
        cycle_fail="cycle $i: destroy of env $env_id failed (ok='$dok' error='$derr')"
        break
    fi
    i=$((i + 1))
done
if [ -n "$cycle_fail" ]; then
    echo "FAILED: the recycle loop did not complete: $cycle_fail" >&2
    fail=1
else
    echo "ok:   $CYCLES create -> destroy cycles all succeeded, each with the environment's two sidecars live in partition $pid"
fi

# ── Phase 5: the name is reusable — the registry entry really went ─────────
# Cycle 1 registered `drv.ramdisk.1`/`aerosls.posix.1` in this partition. The
# kernel refuses a duplicate registration inside one partition (E2 keys the
# registry on name+partition), so creating that index again can only succeed if
# the destroy dropped the entry. This is the roadmap's tooth — "skip the
# registry cleanup — the cycle that exceeds the registry size fails with
# registry full" — made observable without a second kernel build: the same
# assertion, reached from the other end.
reuse_id=""
if [ -z "$cycle_fail" ]; then
    read -r reuse_id rok <<EOF
$(env_create "$pid" 1)
EOF
    if [ "$rok" = "true" ] && [ -n "$reuse_id" ] && [ "$reuse_id" != "0" ]; then
        echo "ok:   index 1 is creatable again after its destroy — the registry entry was really dropped (E2 refuses duplicates in one partition)"
    else
        echo "FAILED: index 1 could not be created again in partition $pid (env_id='$reuse_id' ok='$rok') — a destroyed environment's registry entry or table slot is still held" >&2
        fail=1
    fi
    if [ "$TOOTH" != "skip-destroy" ] && [ -n "$reuse_id" ] && [ "$reuse_id" != "0" ]; then
        read -r dok derr <<EOF
$(env_destroy "$pid" "$reuse_id")
EOF
        [ "$dok" = "true" ] || { echo "FAILED: the re-created environment could not be destroyed (ok='$dok' error='$derr')" >&2; fail=1; }
    fi
fi

# ── Phase 6: the frames came back ──────────────────────────────────────────
# PARTITION_SYSTEM is the leak detector with the widest aperture: an
# environment's 1344 region frames and its sidecars' image/stack frames are all
# charged to its own partition (E4), so a destroy that forgot a region would
# show here as a drift as large as an environment. The tolerance is small
# because the quantity is: HTTP connection buffers and timer work move
# PARTITION_SYSTEM by tens of frames at most, and the environment it must not
# have absorbed is 1500+.
if [ -z "$cycle_fail" ] && [ "$TOOTH" != "skip-destroy" ]; then
    sleep 3   # let a deferred (RUNNING-target) teardown's reap land
    partitions_snapshot; processes_snapshot
    sys_after="$(frame_usage "$W/parts.json" 0)"
    part_after="$(frame_usage "$W/parts.json" "$pid")"
    [ -n "$sys_after" ] || sys_after=0
    [ -n "$part_after" ] || part_after=0
    d_sys=$(( sys_after - sys_before ))
    d_part=$(( part_after - part_before ))
    if [ "$d_sys" -le 64 ] && [ "$d_sys" -ge -64 ]; then
        echo "ok:   PARTITION_SYSTEM's frame count is stable across $CYCLES environments (+$d_sys frames: $sys_before -> $sys_after)"
    else
        echo "FAILED: PARTITION_SYSTEM's frame_usage moved by $d_sys frames across $CYCLES create->destroy cycles ($sys_before -> $sys_after) — an environment's memory did not all come back" >&2
        fail=1
    fi
    if [ "$d_part" -le 64 ] && [ "$d_part" -ge -64 ]; then
        echo "ok:   partition $pid's own frame count is stable too (+$d_part frames: $part_before -> $part_after)"
    else
        echo "FAILED: partition $pid's frame_usage moved by $d_part frames across the loop ($part_before -> $part_after)" >&2
        fail=1
    fi
    left="$(part_env_names "$W/procs.json" "$pid")"
    if [ -z "$left" ]; then
        echo "ok:   no environment sidecar survives in partition $pid"
    else
        echo "FAILED: environment sidecars are still running in partition $pid after every destroy: $left" >&2
        fail=1
    fi
    sys_procs_after="$(proc_count "$W/procs.json" 0)"
    if [ -n "$sys_procs_before" ] && [ -n "$sys_procs_after" ] && [ "$sys_procs_after" = "$sys_procs_before" ]; then
        echo "ok:   PARTITION_SYSTEM's process count returned to $sys_procs_before (slots recycled, nothing respawned)"
    else
        echo "FAILED: PARTITION_SYSTEM's process count moved across the loop ($sys_procs_before -> $sys_procs_after) — a process slot did not come back" >&2
        fail=1
    fi
fi

# ── Phase 7: pause and resume (§8's third bullet) ──────────────────────────
# A paused partition stops SCHEDULING its processes; it does not end them. So
# the assertions have two halves: a placement into it is refused, and the
# environment already in it is still there afterwards — then, after the resume,
# that same environment is still destroyable and its frames still come back.
if [ -z "$cycle_fail" ] && [ "$TOOTH" != "skip-destroy" ]; then
    read -r hold_id hok <<EOF
$(env_create "$pid" 7)
EOF
    if [ "$hok" != "true" ]; then
        echo "FAILED: could not create the environment the pause phase needs (env_id='$hold_id' ok='$hok')" >&2
        fail=1
    else
        api "$W/pause.json" -X POST -d "{\"partition_id\":$pid}" "$BASE/api/partition/pause" || true
        if [ "$(jval "$W/pause.json" ok)" = "true" ]; then
            echo "ok:   POST /api/partition/pause paused partition $pid"
        else
            echo "FAILED: POST /api/partition/pause did not pause partition $pid" >&2
            fail=1
        fi
        read -r pid2 pok <<EOF
$(env_create "$pid" 8)
EOF
        if [ "$pok" != "true" ]; then
            # The refusal is real; its MESSAGE is a known E4 residual (§7.2):
            # every denial on the placement path returns 0 on the ABI, so the
            # API cannot tell a gate refusal from frame-pool exhaustion and
            # reports the latter. Named here so the log carries it.
            echo "ok:   a placement into the PAUSED partition is refused (reported as '$(jval "$W/env.json" error)' — the §7.2 residual: a denial has no code of its own on this ABI)"
        else
            echo "FAILED: an environment was placed into partition $pid while it was paused (env_id='$pid2')" >&2
            fail=1
        fi
        processes_snapshot
        held="$(part_env_names "$W/procs.json" "$pid")"
        if [ "$held" = "aerosls.posix.7 drv.ramdisk.7" ]; then
            echo "ok:   the pause did NOT end the environment already in it — its sidecars are still live (paused stops scheduling, it does not kill)"
        else
            echo "FAILED: the environment that was live before the pause is gone or changed (saw '$held')" >&2
            fail=1
        fi
        api "$W/resume.json" -X POST -d "{\"partition_id\":$pid}" "$BASE/api/partition/resume" || true
        if [ "$(jval "$W/resume.json" ok)" = "true" ]; then
            echo "ok:   POST /api/partition/resume resumed partition $pid"
        else
            echo "FAILED: POST /api/partition/resume did not resume partition $pid" >&2
            fail=1
        fi
        # Serviceable again: and the environment that lived across the pause is
        # still destroyable, with its frames returning to the same partition.
        partitions_snapshot
        part_reuse_before="$(frame_usage "$W/parts.json" "$pid")"
        [ -n "$part_reuse_before" ] || part_reuse_before=0
        read -r dok derr <<EOF
$(env_destroy "$pid" "$hold_id")
EOF
        if [ "$dok" = "true" ]; then
            echo "ok:   the environment that lived across the pause destroyed cleanly after the resume (env $hold_id)"
        else
            echo "FAILED: could not destroy the environment that lived across the pause (ok='$dok' error='$derr')" >&2
            fail=1
        fi
        sleep 2
        partitions_snapshot
        part_reuse_after="$(frame_usage "$W/parts.json" "$pid")"
        [ -n "$part_reuse_after" ] || part_reuse_after=0
        d_reuse=$(( part_reuse_after - part_reuse_before ))
        if [ "$d_reuse" -le 0 ]; then
            echo "ok:   partition $pid's frame count came DOWN after that destroy ($part_reuse_before -> $part_reuse_after) — the regions were released, not stranded by the pause"
        else
            echo "FAILED: partition $pid's frame count did not fall after destroying a post-resume environment ($part_reuse_before -> $part_reuse_after)" >&2
            fail=1
        fi
        processes_snapshot
        if [ -z "$(part_env_names "$W/procs.json" "$pid")" ]; then
            echo "ok:   the post-resume environment left no sidecar behind"
        else
            echo "FAILED: sidecars remain in partition $pid after the post-resume destroy" >&2
            fail=1
        fi
    fi
fi

# ── Phase 8: a partition destroy IS an environment ending (§8's last bullet)
# Create a second partition and an environment in it, then destroy the
# PARTITION — not the environment. `partition destroy` kills every process in
# it, so both of the environment's sidecars simply cease. init must treat that
# as the environment ending: ask it to destroy that environment afterwards and
# it must answer ENV_ERR_NOENT ("no such environment"), not ENV_OK. The
# difference is the assertion: an ENV_OK there would mean init was still
# holding an environment whose sidecars and frames the partition teardown
# already reclaimed, and its destroy would have issued a region release against
# a partition that no longer exists.
if [ -z "$cycle_fail" ] && [ "$TOOTH" != "skip-destroy" ]; then
    api "$W/p2.json" -X POST -d "{\"name\":\"e5gone\"}" "$BASE/api/partitions" || true
    pid2="$(jval "$W/p2.json" partition_id)"
    if [ "$(jval "$W/p2.json" ok)" = "true" ] && [ -n "$pid2" ]; then
        read -r gone_id gok <<EOF
$(env_create "$pid2" 1)
EOF
        if [ "$gok" != "true" ]; then
            echo "FAILED: could not create the environment the partition-destroy phase needs (ok='$gok')" >&2
            fail=1
        else
            processes_snapshot
            if [ -n "$(part_env_names "$W/procs.json" "$pid2")" ]; then
                echo "ok:   partition $pid2 has a live environment (env $gone_id) before its partition is destroyed"
            else
                echo "FAILED: the environment in partition $pid2 is not visible in /api/processes" >&2
                fail=1
            fi
            api "$W/p2destroy.json" -X POST -d "{\"partition_id\":$pid2}" "$BASE/api/partition/destroy" || true
            if [ "$(jval "$W/p2destroy.json" ok)" = "true" ]; then
                echo "ok:   POST /api/partition/destroy destroyed partition $pid2 with an environment in it"
            else
                echo "FAILED: partition $pid2 could not be destroyed" >&2
                fail=1
            fi
            processes_snapshot
            if [ -z "$(part_env_names "$W/procs.json" "$pid2")" ]; then
                echo "ok:   the partition destroy took the environment's sidecars with it"
            else
                echo "FAILED: an environment sidecar survived its partition's destroy" >&2
                fail=1
            fi
            # Ask init to destroy the environment the partition teardown
            # already ended. This request is ALSO the tick on which init
            # notices the ending (the manager sweeps at the top of every ENV
            # request), so the kernel-side evidence below lands here rather
            # than at the partition destroy itself — that ordering is the
            # mechanism, not an artefact, which is why the log assertions come
            # after this call and not before it.
            read -r d2ok d2err <<EOF
$(env_destroy "$pid2" "$gone_id")
EOF
            if [ "$d2ok" = "false" ] && [ "$d2err" = "no such environment" ]; then
                echo "ok:   destroying the environment a partition teardown already ended answers 'no such environment' — init treated the peer-close as an ending"
            else
                echo "FAILED: destroying env $gone_id after partition $pid2 was destroyed answered ok='$d2ok' error='$d2err' — init still holds an environment whose partition is gone" >&2
                fail=1
            fi
            # The kernel's own refusal of a release into a partition that no
            # longer exists. This is kernel TX (it cannot be lost to a console
            # race), and it is the proof the frames were NOT released twice:
            # the partition teardown already returned every frame it owned, so
            # init's attempt is refused BEFORE it touches the frame bitmap.
            if grep -aqF "[FREE_REGION] CAP_EINVAL: target partition $pid2 is not an active partition" "$LOG"; then
                echo "ok:   the kernel refused to release the ended environment's regions into the dead partition (no double free)"
            else
                echo "FAILED: no '[FREE_REGION] CAP_EINVAL: target partition $pid2 is not an active partition' line after the destroy request — init did not try to reclaim the ended environment, or the kernel let it" >&2
                fail=1
            fi
            # init's console path is the lossy sidecar console, so its own
            # line is reported rather than gated (the E3 guard learned that
            # lesson); the HTTP answer above is the gate.
            if grep -aq "ended by their partition's teardown" "$LOG"; then
                echo "note: init logged its own account of the ending ($(grep -a "ended by their partition's teardown" "$LOG" | tail -1 | tr -d '\r'))"
            else
                echo "note: init's own 'ended by their partition's teardown' line was not in the serial log (its console path is the lossy sidecar one; the HTTP answer above is the gate)"
            fi
        fi
    else
        echo "FAILED: could not create the second partition for the partition-destroy phase" >&2
        fail=1
    fi
fi

# ── Verdict ────────────────────────────────────────────────────────────────
echo
if [ "$fail" -ne 0 ]; then
    report_and_exit
fi
echo "PASS  the E5 environment recycle path ($CYCLES create -> destroy cycles over HTTP: table slots, registry entries, process slots and frames all returned)"
exit 0

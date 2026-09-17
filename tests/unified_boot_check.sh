#!/usr/bin/env bash
# tests/unified_boot_check.sh — proves POSIX-Environments E1 on the real target:
# ONE boot in which the Ring-0 control plane (kernel HTTP server on the
# management NIC, plus the kernel shell on the console) and the Phase-5 sidecar
# world (init -> Device Manager -> ramdisk, running in Ring 3) are alive at the
# SAME time, sharing the CPU through the cooperative yield.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E1 is the "unified boot" step of the POSIX-Environments roadmap
# (docs/AeroSLS-POSIX-Environments-Roadmap-v0.1.md §4): before it, the two
# worlds were mutually exclusive by construction — grub entry 0 loads the
# sidecars.cpio initrd and the kernel hands the machine over
# (launch_init_sidecar() enters init via kernel_enter_sidecar and never
# returns, so http_server_run() is unreachable), while entry 1 boots without
# the initrd and never creates a sidecar at all (roadmap G1). The unified boot
# is grub entry 3 (`unified=1` on the command line): the kernel creates the
# init sidecar, LEAVES it runnable, and keeps running its own control plane —
# sharing the CPU by yielding (kernel_yield_to_ring3) at the control plane's
# idle points, with the timer's Ring-3 path (schedule_ring3) handing it back
# when the yield budget expires.
#
# Nothing about that is observable from source alone, and no other guard boots
# this entry: it takes a real guest to show that the pseudo-process is planted,
# that init actually gets scheduled without the kernel hand-over, that the
# kernel gets its CPU back, that init keeps making progress while the HTTP API
# answers, and that the hardware half of the Phase-5 chain stays unspawned
# (the kernel owns the NICs and the console in this boot, so the POSIX sidecar
# and the network driver must NOT be created — they would fight the kernel for
# the same hardware). Each of those is one missing line in the serial log, so
# this check reads the log.
#
# The assertions are deliberately behavioural, not decorative: the heartbeat
# counter can ONLY advance while BOTH halves work (init needs the kernel to
# yield in order to run at all, and needs the Ring-3 timer path to preempt it
# back for the next heartbeat), and the /api/health probes are taken around a
# heartbeat sample, so "both alive at once" is measured rather than assumed.
#
# Prerequisite: sls_operating_system.iso (make x86-iso, with sidecars.cpio
# present — commit it so CI ships it) and qemu-system-x86_64. Reads nothing
# else; touches only its temp log.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Teeth: tests/unified_boot_check_smoke.sh points this guard at a boot that
# does not have the property (the kernel-only entry) and requires it to FAIL.
#
# Env knobs (used by the smoke; the defaults are what every other caller gets):
#   UNIFIED_BOOT_ENTRY     1-based grub MENU POSITION to boot (default 3 = the
#                          unified entry; 2 is the kernel-only boot the smoke
#                          sabotages this guard with)
#   UNIFIED_BOOT_WINDOW_S  seconds to wait for the markers  (default 120)
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first),
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="sls_operating_system.iso"
LOG=/tmp/aerosls_unified_boot.log
ENTRY="${UNIFIED_BOOT_ENTRY:-3}"      # 1 = Phase-5 initrd boot, 2 = kernel-only,
                                      # 3 = unified — the 1-based menu position
                                      # tests/grub_select_kernel_only.sh boots by
                                      # sending ENTRY-1 Down keys
WINDOW_S="${UNIFIED_BOOT_WINDOW_S:-120}"

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }

SER=/tmp/aerosls_unified_ser
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

# No disk is attached ON PURPOSE: this check asserts nothing about storage, and
# a private fresh image would only add a second variable (the kernel's NVMe
# bring-up and its TLS-CA persistence path) to a boot whose subject is CPU
# sharing. The boot is exercised without one by the kernel-only entry too.
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
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
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

# ── Phase 1: wait for the boot to be far enough along to assert on ──────────
# Bounded by WINDOW_S and by QEMU's liveness. The two contradiction branches
# below are the guard's honesty checks: a boot that is not the unified boot is
# this guard pointed at the wrong thing (the smoke's sabotage, or a grub menu
# whose order moved), and they name that as soon as the kernel makes it
# visible instead of waiting out the window for the same verdict.
ready=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S * 2))); do
    # Contradiction 1: a non-empty command line that does not ask for the
    # unified boot. (The kernel prints this line only when the command line is
    # non-empty, so it is evidence when it appears, not a prerequisite.)
    if grep -aq "\[BOOT\] command line:" "$LOG" 2>/dev/null &&
       ! grep -aq "unified=1" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY booted without unified=1 on the kernel command line (this is not the unified boot):" >&2
        grep -a "\[BOOT\] command line:" "$LOG" | head -1 | sed 's/^/      /' >&2
        exit 1
    fi
    # Contradiction 2: the control plane is planted (kernel_main, step 7d-ante,
    # before launch_init_sidecar) strictly before the HTTP listener starts
    # (step 8), so a boot that reaches its listener without the planting line is
    # not the unified boot — that is the kernel-only entry, and waiting out the
    # window would only reach the same verdict with a weaker diagnostic.
    if grep -aq "Listening on port 3000" "$LOG" 2>/dev/null &&
       ! grep -aq "\[E1\] control plane planted" "$LOG" 2>/dev/null; then
        echo "FAILED: grub menu entry $ENTRY reached its HTTP listener without planting the Ring-0 control plane — this is not the unified boot" >&2
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
marker() {   # marker <description> <fixed string>
    if grep -aqF "$2" "$LOG" 2>/dev/null; then
        echo "ok:   $1"
    else
        echo "FAILED: $1 (no '$2' in the serial log)" >&2
        fail=1
    fi
}
absent() {   # absent <description> <fixed string>
    if grep -aqF "$2" "$LOG" 2>/dev/null; then
        echo "FAILED: $1 (found '$2')" >&2
        fail=1
    else
        echo "ok:   $1"
    fi
}

# One exit path, so every failure prints the same evidence.
report_and_exit() {
    echo "FAIL  the unified boot (control plane + Ring-3 sidecars in one boot) is not working as E1 requires" >&2
    echo "      E1/INIT lines seen:" >&2
    grep -a "\[E1\]\|\[INIT\]" "$LOG" 2>/dev/null | head -20 | sed 's/^/      /' >&2
    echo "      last 20 serial lines:" >&2
    tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
    exit 1
}

# ── Phase 2: the boot shape ─────────────────────────────────────────────────
marker "the kernel parsed unified=1 from the command line" "[BOOT] unified boot requested (unified=1)"
marker "the Ring-0 control plane was planted as a schedulable pseudo-process" "[E1] control plane planted as PID 1"
marker "the init sidecar was created in this boot" "[E1] init sidecar created (PID "
marker "init was LEFT RUNNABLE instead of being entered (the kernel kept the boot)" ") and left RUNNABLE"
marker "init was released by the sidecar creator (PROC_HELD -> PROC_SUSPENDED)" "released — runs on next schedule"
# The BIB flag is asserted from the KERNEL's own line, not init's own "[INIT]
# UNIFIED boot" console line. init's line is real evidence, but it reaches the
# log through its console channel, which the AP core drains — and in the
# unified boot that drain shares COM1 with the BSP's control-plane prints, so a
# sidecar line can arrive split across another writer's bytes (observed before
# the kernel's TX serializer landed: "[INIT] UNIF<kernel line>IED boot"). The
# kernel's own statement of the flag it stamped is kernel TX and is therefore
# written whole. init's HAVING READ the flag stays asserted, from the other
# side: Phase 3 requires the POSIX sidecar and the network/e1000 drivers to be
# UNSPAWNED, which is exactly what reading UNIFIED makes init do.
marker "the kernel stamped the BIB's UNIFIED flag into every sidecar's boot info block" "[E1] BIB v2 flags=0x1 (UNIFIED)"
marker "the kernel owns the console in this boot (input forwarding off)" "[CONSOLE] sidecar input forwarding disabled"

# ── Phase 3: device ownership — the hardware half stays unspawned ───────────
# Asserted through the KERNEL's own sidecar-creation lines (each created child
# is announced as `[SIDECAR] PID <n> '<manifest name>'`), NOT through init's
# console text: init's log lines reach the serial port through its console
# channel and are legitimately interleaved with the kernel's own prints
# mid-line (the unified boot has two writers by construction), so a long
# console-derived substring is not a stable thing to grep for. The sidecar
# names are the same contract the manifests declare, and the software half is
# asserted positively so an empty log cannot satisfy this phase.
marker "the software half of the chain ran (init's own ramdisk driver was created)" "'drv.ramdisk.0'"
absent "no system POSIX sidecar was created (it would fight the kernel for the NIC and COM1)" "aerosls.posix.0"
absent "no network driver sidecar was created (the kernel drives the NIC in this boot)" "drv.network.0"
absent "no e1000 user-driver sidecar was created (no NIC was handed off)" "drv.e1000.0"

# ── Phase 4: the CPU really is shared ───────────────────────────────────────
if [ "$ready" -eq 1 ]; then
    echo "ok:   the boot reached the unified markers (heartbeats and the HTTP listener)"
else
    # Nothing later can pass once the boot itself is not a unified boot, so
    # report now instead of spending the HTTP/console waits on a dead premise
    # (a boot that produced none of Phase 2's lines never had a control plane
    # to answer HTTP or a shell to answer the console).
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAILED: QEMU exited before the unified boot reached its markers — the boot crashed" >&2
    else
        echo "FAILED: markers missing within ${WINDOW_S}s (heartbeats: $(hb_now), yields: $(grep -ac '\[E1\] yield' "$LOG" 2>/dev/null))" >&2
    fi
    report_and_exit
fi

# The yield lines are the kernel handing the CPU to Ring-3 work; the heartbeat
# lines are init, running in Ring 3, reporting progress. Requiring several of
# each, with heartbeats AFTER yields (init ran because the kernel yielded) and
# yields AFTER heartbeats (the kernel got the CPU back after init ran), is the
# round-robin property stated as data rather than as prose.
yields=$(grep -ac "\[E1\] yield: control plane -> PID" "$LOG" 2>/dev/null || true)
hbs=$(grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true)
hb_after_yield=$(awk '/\[E1\] yield/ {y=1} /\[INIT\] heartbeat/ && y {n++} END {print n+0}' "$LOG")
yield_after_hb=$(awk '/\[INIT\] heartbeat/ {h=1} /\[E1\] yield/ && h {n++} END {print n+0}' "$LOG")
if [ "${yields:-0}" -ge 3 ] && [ "${hbs:-0}" -ge 5 ]; then
    echo "ok:   the kernel yielded to Ring-3 work $yields times and init reported $hbs heartbeats"
else
    echo "FAILED: the two halves did not round-robin (yields=$yields, heartbeats=$hbs)" >&2
    grep -a "\[E1\]\|\[INIT\]" "$LOG" | head -20 | sed 's/^/      /' >&2
    fail=1
fi
if [ "$hb_after_yield" -ge 3 ] && [ "$yield_after_hb" -ge 3 ]; then
    echo "ok:   the CPU alternated (heartbeats after yields: $hb_after_yield; yields after heartbeats: $yield_after_hb)"
else
    echo "FAILED: no alternation between the control plane and Ring-3 (heartbeats after yields=$hb_after_yield, yields after heartbeats=$yield_after_hb)" >&2
    fail=1
fi

# ── Phase 5: both halves alive AT ONCE (the E1 property) ────────────────────
# Sample init's heartbeat counter, then serve two HTTP requests through the
# control plane, then sample again: the counter must have advanced across the
# requests. On a Phase-5 boot there are no heartbeats at all; on a kernel-only
# boot there is no init. This is the assertion the two old boot modes cannot
# satisfy by construction.
# Each probe retries: a single refused connection is a host-side hiccup (the
# guest's accept path is Ring-0 work on a shared CPU), while a control plane
# that is genuinely not serving fails every attempt.
probe_health() {   # probe_health <out-file>
    for _t in 1 2 3 4 5 6; do
        if curl -sf --max-time 10 -o "$1" "$BASE/api/health" 2>/dev/null; then
            return 0
        fi
        sleep 2
    done
    return 1
}
hb_before=$(hb_now)
health1=0
health2=0
probe_health "$W/h1.json" && health1=1
sleep 2
probe_health "$W/h2.json" && health2=1
hb_after=$(hb_now)
if [ "$health1" -eq 1 ] && [ "$health2" -eq 1 ] && [ "$hb_after" -gt "$hb_before" ]; then
    echo "ok:   /api/health answered twice while init was making progress (heartbeats $hb_before -> $hb_after)"
else
    echo "FAILED: both halves were not alive at once (health=$health1/$health2, heartbeats $hb_before -> $hb_after)" >&2
    fail=1
fi
# The health BODY, not just the status code: a 200 from a degraded service
# would otherwise pass (same discipline as CI's own /api/health step).
if [ "$health2" -eq 1 ] && ! python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
if d.get("status") != "ok":
    sys.exit("status is %r, expected \"ok\"" % d.get("status"))
u = d.get("uptime_ticks")
if isinstance(u, bool) or not isinstance(u, int) or u <= 0:
    sys.exit("uptime_ticks is %r, expected a positive integer" % u)
' "$W/h2.json"; then
    echo "FAILED: /api/health answered, but its body failed the status/uptime assertion" >&2
    fail=1
else
    echo "ok:   /api/health body is a live kernel (status=ok, uptime_ticks>0)"
fi

# ── Phase 6: the control plane owns the console ─────────────────────────────
# In the unified boot the kernel shell and the sidecar world share this boot,
# and console ownership was decided in the kernel's favour (init's manifests
# carry the COM1 port I/O, so a sidecar shell would steal the keystrokes). Type
# a command into the guest's serial console and require the KERNEL shell to
# answer it: the round trip only exists while the control plane's poll runs.
printf 'help\r' > "$SER.in" 2>/dev/null || true
console_ok=0
for i in $(seq 1 40); do   # up to 20 s
    if grep -aqF "SLS Shell Commands:" "$LOG" 2>/dev/null; then
        console_ok=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 0.5
done
if [ "$console_ok" -eq 1 ]; then
    echo "ok:   typed input was answered by the kernel shell (the control plane owns the console)"
else
    echo "FAILED: the kernel shell did not answer a typed 'help' — the control plane's console poll is not running" >&2
    fail=1
fi

# ── Stop the guest (bounded: SIGTERM, 10 s grace, SIGKILL) ──────────────────
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

echo "OK: the unified boot ran the Ring-0 control plane (HTTP + shell) and the Ring-3 sidecar world in one boot"
exit 0

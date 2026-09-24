#!/usr/bin/env bash
# tests/env_console_attach_check.sh — proves POSIX-Environments E6 on the real
# target: two environments in one partition are each reachable through their
# own console, and NEITHER stream ever carries a byte the other environmental
# produced.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E6 is the phase that makes an environment reachable at all (G7: "an
# environment you cannot reach is not an environment"), and the roadmap
# (docs/AeroSLS-POSIX-Environments-Roadmap-v0.1.md §9) names this check
# exactly:
#
#   "An HTTP end-to-end check: create two environments, attach to both, send
#    `echo hi-$ENV | cat` to each, and assert each stream contains only its own
#    output. Tooth: cross-wire the two consoles — the isolation assertion
#    fails."
#
# This is that check. The interesting word is "only": every environment's
# console has existed since E3 in the sense that its output reached *somebody*
# (the kernel log, through the shared `kernel.debug.console`), so a check that
# only asked "does attach see the environment's output" would have passed
# before E6 was written. What E6 adds — and what this guard is the evidence for
# — is that the two streams are DISJOINT: environment 1's output is in
# environment 1's stream, environment 2's output is in environment 2's stream,
# and the two never trade bytes.
#
# ─── What it asserts, and why each one needs a boot ────────────────────────
#   1. GET /api/health answers with a live body, and POST /api/partitions
#      defines the partition the two environments are placed in.
#   2. Two environments are created in it — same partition, so the isolation
#      this guard proves is between two environments that share every other
#      resource (frames, process pool, registry), not between two partitions
#      that a machine boundary would separate for free.
#   3. Both consoles are LIVE and ATTRIBUTED: GET /api/partition/{id}/env
#      reports `live` 2, an `env_id` bound for each (0 is a console no manager
#      bound — an E3 boot spawn — and attach by id cannot address those), the
#      two entries carry distinct `index`es and distinct `posix_pid`s, and each
#      entry's `posix_pid` is, in GET /api/processes, the process NAMED
#      `aerosls.posix.<index>`.
#      That last clause is an identity tie between two kernel surfaces, and the
#      only one available without an in-band self-report: the kernel builds an
#      environment's identity out of the sidecar's own name (env_console.c
#      parses the index out of `aerosls.posix.<index>`), so the name of the
#      process holding a console's far end must agree with the index the
#      registry reports for it. Measured: an image whose listing reports the
#      SIBLING environment's `posix_pid` fails here and only here — both streams
#      are clean under that mutant, because a listing that lies about which
#      sidecar a console belongs to does not change which sidecar it is — while
#      an image that crosses the two consoles at REGISTRATION fails earlier, at
#      this census, naming environment 1's console as never bound. Both are
#      recorded in tests/env_console_attach_check_smoke.sh's source controls.
#   3b. Each console announces its OWN identity: the environment's own boot line
#      (`[env-id] index=<index> pid=<pid>`, written by the sidecar onto its own
#      console — user/sidecar/src/boot.rs, and only for a console the kernel
#      registered as an ENVIRONMENT console, so a sidecar on the kernel
#      transcript stays silent) arrives on the address that
#      environment is attached at, with the pid the census above reports for
#      it, and the other environment's announcement does NOT. This is the
#      assertion that closes the residual below: every other check here reads
#      the two addresses and can be satisfied by a consistent crossing of them,
#      but the announcement is a claim the ENVIRONMENT makes about itself, and
#      it reaches the kernel on a different path than the address a reader
#      looks it up by. Measured: the rebuilt kernel whose `ec_find_env()`
#      returns a sibling console in the same partition — which passed this
#      guard end to end before this clause existed — now fails here, alone,
#      naming both streams' announcements. Recorded in the smoke's header.
#   4. The roadmap's command: `setenv ENV <n>` then `echo hi-$ENV | cat`,
#      the SAME command text on both consoles. Distinctness therefore comes
#      from the environment, not from the typed bytes: environment 1's stream
#      must show `hi-1` and environment 2's `hi-2`. A guard that typed
#      different text at each console could not tell "the kernel delivered my
#      line to the environment I addressed" from "the kernel delivered it to
#      the other one and I read that one back".
#   5. The literal-variant probe: `echo hi-env1 | cat` / `echo hi-env2 | cat`.
#      Where (4) proves the marker came out of the addressed environment, this
#      proves the addressed environment received the bytes meant for it —
#      crossing the INPUT halves moves these markers, and crossing the OUTPUT
#      halves moves (4)'s. Both halves are probed, one marker family each.
#   6. Neither stream carries the other's marker, in either family, and both
#      `dropped` counters are 0 across the whole run. The second half of that
#      matters: `env_console.c` keeps the NEWEST 4 KiB and counts what fell off
#      the front, so "the other environment's bytes are not in my stream" is
#      only meaningful while nothing was dropped — otherwise a commingled
#      stream could pass by forgetting the evidence.
#   6b. A NEIGHBOUR PARTITION, held paused, is the witness this phase needs
#      (code phase 6b, between the two marker phases and the destroys below).
#      Phases 6-7 prove the two consoles of one partition are disjoint, and
#      clause 3b proves each address carries its own environment's identity;
#      neither can speak about an environment in ANOTHER partition, because
#      both of its failsafes are inside the pair. So: a second partition with
#      its own environment — deliberately given the SAME index (1), since the
#      index is not the address — is placed, proven live (its own identity
#      announcement, its own marker derived from its own shell variable), and
#      then its PARTITION is paused (POSIX-Environments E4's route:
#      `POST /api/partition/pause`; kernel/partition.c excludes a paused
#      partition from the scheduling rotation, so the environment stops
#      consuming its console). That pause is what turns a leaked, crossed or
#      partition-blind address into an observable difference:
#        - while paused, a line addressed to the neighbour is accepted only
#          into an empty queue, and the NEXT line is refused (`queued` 0)
#          because the peer has not drained the first — the inverse of clause
#          8's signal, and a state a LIVE environment cannot stay in;
#        - partition {id}'s environment keeps working meanwhile, and neither
#          stream carries the other's marker;
#        - when the neighbour's partition is RESUMED, the line queued during
#          the pause is consumed by the NEIGHBOUR (its marker appears on the
#          neighbour's stream, never on partition {id}'s) — so the address was
#          the neighbour's all along, and the pause was a pause, not a death;
#        - and neither environment is reachable through the other's address at
#          all: both the read and the write half of a (partition, env_id) pair
#          that does not exist in that partition answer "no such environment
#          console in this partition".
#      Tooth: E6_TOOTH=skip-pause withholds the pause, so the neighbour keeps
#      draining and the paused-environment clauses MUST fail. That tooth needs
#      no kernel build, so it runs on every kernel-guards pass.
#      A refusal family is also a family that can pass vacuously on a route that
#      refuses everything, so clause 6b makes the same read once on a pair that
#      DOES exist in its partition and requires it to answer — the non-vacuity
#      control sits next to the claim it protects.
#      MEASURED: skip-pause exits 1 with the two paused-peer clauses red and
#      nothing else; and a rebuilt kernel whose `ec_find_env()` matches on
#      `env_id` alone — a partition-blind registry, the plausible way this could
#      really break — exits 1 on the four cross-partition probes plus the
#      'hi-cross' delivery marker, with the pause family, the census, the
#      attribution tie, the identity clauses and phase A all still GREEN. So the
#      two families catch different mistakes, which is why both are here. Both
#      transcripts are recorded in tests/env_console_attach_check_smoke.sh.
#   7. A destroyed environment's console does not linger. Destroy environment 1
#      over the E5 route and its console leaves the listing and stops
#      answering, while environment 2 keeps working and keeps its isolation.
#      This is env_console.h's own claim ("a destroyed environment's console
#      cannot linger") asserted rather than asserted-about, and it is the only
#      phase here that runs the registry's teardown.
#   8. The egress half of the attach surface: `queued` is the kernel's flow
#      control (a line is refused while the peer has not drained the previous
#      one, so an environment that never reads its console cannot exhaust the
#      payload pool on behalf of the others). Every send below waits for a
#      non-zero `queued` rather than assuming it, so a console that silently
#      swallows input fails here instead of timing out in the marker checks
#      with no reason given.
#
# ─── The mutual-swap gap, and how it was closed ────────────────────────────
# This guard proves the two streams are disjoint and that each address reaches
# exactly one live environment. That alone does NOT prove the two addresses are
# correctly LABELLED relative to each other, because a mutual swap commutes with
# everything a reader does through them: if every byte addressed to 1 were
# delivered to 2, and every byte read back from 1 came from 2, each address
# would still reach exactly one environment, no stream would commingle, and
# every marker assertion below would still hold — the shell's variables, its
# filesystem and its output all follow the address, consistently. That was
# MEASURED, not reasoned: a rebuilt kernel whose `ec_find_env()` returns a
# sibling console in the same partition (kernel/env_console.c) passed this guard
# end to end, and the case was written down here rather than implied away
# because the tooth below crosses ONE half — the case a reader means by
# "cross-wire the two consoles" — and a reader deserved to know which case that
# left unproven.
#
# It is no longer unproven. The distinguishing fact is not how a console
# behaves but what it says it is, and the environment itself supplies it: each
# sidecar announces its own index and pid on its own console at boot (BIB v3's
# identity words, which the kernel fills with values only IT has — the pid it
# just gave that sidecar, and the index it parsed out of that sidecar's own
# NAME record), and clause 3b asserts the announcement arrives on the address
# the census tied to that environment. The swap cannot hide it: the
# announcement reaches the kernel's console registry on the sidecar's own
# output channel, which is filed by the entry that channel belongs to, while a
# reader reaches a console by (partition, env_id) — so the claim travels by the
# path the mutant did not touch and is read by the path it did. RE-MEASURED on
# that same mutant: the guard now exits 1, red on the four identity clauses and
# on NOTHING else — the census, the attribution tie and phase A's four stream
# clauses all stay green under it, which is what "consistent" means here. The
# transcript and the mutant are recorded in the smoke's header (CONTROL C,
# measured both before and after).
#
# Prerequisite: sls_operating_system.iso (make x86-iso; the unified entry needs
# sidecars.cpio present — commit it, so CI ships it) and qemu-system-x86_64.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU).
#
# Teeth: tests/env_console_attach_check_smoke.sh — it points this guard at
# inputs that do NOT have the E6 property (the kernel-only boot, which has no
# environment manager at all; E6_TOOTH=cross-wire, which delivers every command
# to the other environment's console; E6_TOOTH=no-input, which withholds the
# commands so both streams stay empty; and E6_TOOTH=skip-pause, which leaves the
# neighbour partition live so the paused-peer clauses of phase 6b have to fail)
# and requires this guard to go red on each, then requires it to pass on the real
# input. The no-input arm is not decoration: "no stream carries foreign output"
# is trivially true of two empty streams, and a guard that only checked that
# would pass on an environment nothing can run on — the exact defect this phase
# was written to fix. skip-pause is the same kind of arm for the same reason one
# level out: clause 6b rests on a frozen peer, and a frozen peer nobody can
# produce is a clause nobody has shown to be doing any work.
#
# Env knobs (used by the smoke; the defaults are what every other caller gets):
#   E6_ISO            path to the ISO to boot      (default sls_operating_system.iso)
#   E6_BOOT_ENTRY     1-based grub MENU POSITION   (default 3 = the unified entry)
#   E6_WINDOW_S       seconds to wait for the boot markers (default 180)
#   E6_SMP            QEMU -smp                    (default 4)
#   E6_ATTACH_WAIT_S  seconds to wait for a command's output to appear (default 45)
#   E6_DESTROY_WAIT_S seconds to wait for a destroyed console to leave (default 45)
#   E6_TOOTH          smokes only:
#                       cross-wire — deliver every command to the OTHER
#                                    environment's console (the two consoles'
#                                    input paths crossed). Each stream then
#                                    carries the other environment's output, so
#                                    the isolation assertion MUST fail.
#                       no-input   — withhold the commands: both streams stay
#                                    empty, so the own-output assertion MUST
#                                    fail (the vacuity control).
#                       skip-pause — do not pause the neighbour partition in
#                                    phase 6b: the neighbour keeps draining, so
#                                    the clauses that need a frozen peer MUST
#                                    fail (and only those).
#
# Exit: 0 if every assertion held, 1 if one failed (or QEMU died first),
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E6_ISO:-sls_operating_system.iso}"
LOG=/tmp/aerosls_env_console_boot.log
ENTRY="${E6_BOOT_ENTRY:-3}"      # 1 = Phase-5 initrd boot, 2 = kernel-only,
                                 # 3 = unified (the 1-based menu position
                                 # tests/grub_select_kernel_only.sh boots by
                                 # sending ENTRY-1 Down keys)
WINDOW_S="${E6_WINDOW_S:-180}"
ATTACH_WAIT_S="${E6_ATTACH_WAIT_S:-45}"
DESTROY_WAIT_S="${E6_DESTROY_WAIT_S:-45}"
SMP="${E6_SMP:-4}"
TOOTH="${E6_TOOTH:-}"

# The API's own token/role model: DB_ADMIN, which is what partition create and
# an environment's create, destroy and console write all require
# (tools/aeroslsctl's default token — the same one the other HTTP-driven boot
# checks use). The console READ is ungated, like GET /api/partitions.
TOKEN=deadbeef01234567cafebabe76543210

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

SER=/tmp/aerosls_env_console_ser
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
# and the E4/E5 guards make and for the same reason: this check asserts nothing
# about storage, and a private fresh image would add the kernel's NVMe bring-up
# and TLS-CA persistence path to a boot whose subject is console isolation.
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
    -device e1000,netdev=net0,mac=52:54:00:12:34:06 \
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
# Bounded by WINDOW_S and by QEMU's liveness. The two contradiction branches are
# the guard's honesty checks (same shape as unified_boot_check.sh): a boot that
# is not the unified boot is this guard pointed at the wrong thing, and they
# name that as soon as the kernel makes it visible instead of waiting out the
# window for the same verdict.
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
    echo "FAIL  the E6 per-environment console attach path (two environments, disjoint streams)" >&2
    echo "      ENV-CONSOLE / CONSOLE / POSIX / ENV lines seen:" >&2
    grep -a "\[ENV-CONSOLE\]\|\[CONSOLE\]\|\[POSIX\]\|\[ENV\]" "$LOG" 2>/dev/null | tail -40 | sed 's/^/      /' >&2
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
    # leaves the PREVIOUS request's body in place and the caller reads its own
    # stale success — measured in the E5 guard, where the recycle loop reported
    # `ok=true` for a request the console log showed was never made. An empty
    # file makes every caller's `ok` empty, which is not `true`.
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
env_field() {   # env_field <envs.json> <env_id> <field> — that console's field
    # The console listing is the KERNEL's view (kernel/env_console.c), not a
    # second table in init: `env_id` is the manager's id, `index` is the
    # environment's identity within its partition, `posix_pid` is the sidecar
    # holding the far end. Both query shapes below need to ask "what does the
    # kernel say about environment N", so the lookup lives in one place.
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
proc_name_of() {   # proc_name_of <processes.json> <pid> — the process's name
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
for p in d.get("processes", []):
    if str(p.get("pid")) == sys.argv[2]:
        sys.stdout.write(str(p.get("name", "")))
        break
PY
}
console_get() {   # console_get <partition> <env_id> — "<ok> <dropped>"; body in $W/cons.json
    local part="$1" id="$2"
    api "$W/cons.json" "$BASE/api/partition/$part/env/$id/console" || true
    printf '%s %s\n' "$(jval "$W/cons.json" ok)" "$(jval "$W/cons.json" dropped)"
}
console_out() {   # stdout: the `output` field of $W/cons.json, verbatim
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
partitions_snapshot() { api "$W/parts.json" "$BASE/api/partitions" || true; }
processes_snapshot()  { api "$W/procs.json" "$BASE/api/processes"  || true; }
envs_snapshot()       { api "$W/envs.json" "$BASE/api/partition/$pid/env" || true; }

# ── Phase 2: the control plane answers with a live body ────────────────────
api "$W/health.json" "$BASE/api/health" || true
if [ "$(jval "$W/health.json" status)" = "ok" ]; then
    echo "ok:   /api/health answers with a live kernel body"
else
    echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
    fail=1
fi

# ── Phase 3: a partition over HTTP — the placement target ──────────────────
pname="e6attach"
api "$W/pcreate.json" -X POST -d "{\"name\":\"$pname\"}" "$BASE/api/partitions" || true
pid="$(jval "$W/pcreate.json" partition_id)"
if [ "$(jval "$W/pcreate.json" ok)" = "true" ] && [ -n "$pid" ] && [ "$pid" != "0" ] && [ "$pid" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined '$pname' as partition $pid"
else
    echo "FAILED: POST /api/partitions did not define a partition (id='$pid')" >&2
    jval "$W/pcreate.json" error >&2 || true
    report_and_exit
fi

# ── Phase 4: two environments in it ────────────────────────────────────────
# Same partition, deliberately: two environments that share a frame pool, a
# process table and a sidecar registry are the case where a console wired to
# the wrong owner is possible at all.
env_create() {   # env_create <partition> <index> — echoes "<env_id> <ok>"
    local part="$1" idx="$2"
    api "$W/env.json" -X POST -d "{\"index\":$idx}" "$BASE/api/partition/$part/env" || true
    printf '%s %s\n' "$(jval "$W/env.json" env_id)" "$(jval "$W/env.json" ok)"
}
read -r E1 ok1 <<EOF
$(env_create "$pid" 1)
EOF
read -r E2 ok2 <<EOF
$(env_create "$pid" 2)
EOF
if [ "$ok1" = "true" ] && [ -n "$E1" ] && [ "$E1" != "0" ]; then
    echo "ok:   environment 1 placed in partition $pid (env_id $E1, index 1)"
else
    echo "FAILED: could not create environment 1 in partition $pid (env_id='$E1' ok='$ok1' error='$(jval "$W/env.json" error)')" >&2
    fail=1
fi
if [ "$ok2" = "true" ] && [ -n "$E2" ] && [ "$E2" != "0" ]; then
    echo "ok:   environment 2 placed in partition $pid (env_id $E2, index 2)"
else
    echo "FAILED: could not create environment 2 in partition $pid (env_id='$E2' ok='$ok2' error='$(jval "$W/env.json" error)')" >&2
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    report_and_exit
fi
if [ "$E1" = "$E2" ]; then
    echo "FAILED: both environments came back with the same env_id ($E1) — attach cannot tell them apart" >&2
    report_and_exit
fi

# ── Phase 5: both consoles are live, bound and attributed ──────────────────
# Bounded: the sidecars are created by the environment manager over a channel
# round trip, and the kernel binds the manager's id to the console when the
# ENV_CREATE reply arrives, so a console that is `env_id 0` moments after the
# create is a console mid-flight, not a broken one.
bound=0
for i in $(seq 1 120); do
    envs_snapshot
    if [ "$(jval "$W/envs.json" live)" = "2" ] &&
       [ -n "$(env_field "$W/envs.json" "$E1" index)" ] &&
       [ -n "$(env_field "$W/envs.json" "$E2" index)" ]; then
        bound=1
        break
    fi
    sleep 0.5
done
if [ "$bound" -ne 1 ]; then
    echo "FAILED: GET /api/partition/$pid/env never reported both environments' consoles (live='$(jval "$W/envs.json" live)', wanted 2; env $E1 index='$(env_field "$W/envs.json" "$E1" index)', env $E2 index='$(env_field "$W/envs.json" "$E2" index)')" >&2
    report_and_exit
fi

processes_snapshot
i1="$(env_field "$W/envs.json" "$E1" index)"; p1="$(env_field "$W/envs.json" "$E1" posix_pid)"
i2="$(env_field "$W/envs.json" "$E2" index)"; p2="$(env_field "$W/envs.json" "$E2" posix_pid)"
n1="$(proc_name_of "$W/procs.json" "$p1")"
n2="$(proc_name_of "$W/procs.json" "$p2")"
# The identity tie: the kernel derived each console's index from the sidecar's
# OWN NAME (env_console.c parses `aerosls.posix.<index>`), so the index in the
# console registry and the name of the process holding the far end must agree.
# This is the assertion a systematically mis-attributed registry fails while
# its streams still look self-consistent — see the header's residual note.
if [ "$i1" = "1" ] && [ "$n1" = "aerosls.posix.1" ] &&
   [ "$i2" = "2" ] && [ "$n2" = "aerosls.posix.2" ]; then
    echo "ok:   both consoles are live and attributed: env $E1 -> index 1, posix pid $p1 ('$n1'); env $E2 -> index 2, posix pid $p2 ('$n2')"
else
    echo "FAILED: the console registry and /api/processes disagree about which sidecar owns a console (env $E1 -> index '$i1' pid '$p1' name '$n1'; env $E2 -> index '$i2' pid '$p2' name '$n2')" >&2
    fail=1
fi
if [ "$p1" != "$p2" ] && [ -n "$p1" ] && [ -n "$p2" ]; then
    echo "ok:   the two consoles belong to different sidecars (pids $p1 and $p2)"
else
    echo "FAILED: both consoles report the same posix_pid ('$p1' / '$p2')" >&2
    fail=1
fi
# The kernel's own account of the wiring, from the boot log: printed by
# env_console_register() at the moment the peer was minted, so it is an
# independent witness to the attribution above (and, being kernel TX, it cannot
# be lost to a console race). Reported rather than gated: the HTTP listing above
# is already a kernel-side answer, and a guard that gates twice on one fact
# reports a failure twice.
for pair in "1 $p1" "2 $p2"; do
    idx="${pair%% *}"; spid="${pair##* }"
    if grep -aqF "[ENV-CONSOLE] environment (partition $pid, index $idx) console wired: posix pid $spid" "$LOG"; then
        echo "note: the kernel's wiring line agrees: index $idx was wired by posix pid $spid"
    fi
done

# ── Console plumbing ───────────────────────────────────────────────────────
# S1/S2 accumulate each environment's output across the whole run (a read is
# destructive — the kernel hands bytes over once), and D1/D2 latch the dropped
# counters. Markers are distinct per phase so a later phase cannot satisfy an
# earlier phase's assertion.
S1=""; S2=""; D1="0"; D2="0"

# drain_once: everything below reads a console through this, and it is the only
# place that consumes bytes. C_OK / C_OUT / C_DROP are its results.
drain_once() {   # drain_once <partition> <env_id>
    local part="$1" id="$2" r
    r="$(console_get "$part" "$id")"
    C_OK="${r%% *}"
    C_DROP="${r##* }"
    C_OUT=""
    if [ "$C_OK" = "true" ]; then
        C_OUT="$(console_out)"
    fi
}
drain_both() {   # drain both, appending to S1/S2; 1 if either console is gone
    local gone=0
    drain_once "$pid" "$E1"
    if [ "$C_OK" = "true" ]; then S1="$S1$C_OUT"; D1="$C_DROP"; else gone=1; fi
    drain_once "$pid" "$E2"
    if [ "$C_OK" = "true" ]; then S2="$S2$C_OUT"; D2="$C_DROP"; else gone=1; fi
    [ "$gone" = 0 ]
}
wait_for() {   # wait_for <1|2> <marker> <seconds> — 0 once that stream shows it
    local which="$1" marker="$2" secs="$3" i
    for i in $(seq 1 $((secs * 2))); do
        drain_both
        if [ "$which" = 1 ]; then
            case "$S1" in *"$marker"*) return 0 ;; esac
        else
            case "$S2" in *"$marker"*) return 0 ;; esac
        fi
        sleep 0.5
    done
    return 1
}
# The tooth's addressing. `cross-wire` delivers every command the guard means
# for environment 1 to environment 2's console and vice versa; the reads stay on
# their own address, so each stream ends up carrying the other environment's
# output — the observable state of a kernel whose input halves are crossed.
send_addr() {   # send_addr <1|2> — the env_id a command for that environment goes to
    if [ "$TOOTH" = "cross-wire" ]; then
        if [ "$1" = 1 ]; then printf '%s\n' "$E2"; else printf '%s\n' "$E1"; fi
    else
        if [ "$1" = 1 ]; then printf '%s\n' "$E1"; else printf '%s\n' "$E2"; fi
    fi
}
# ── Phase 5b: each console announces its OWN identity at boot ─────────────
# The assertion the census above cannot make, and the one that closes this
# guard's stated residual. Every check so far reads the two addresses; if both
# directions were crossed at once (a byte addressed to 1 delivered to 2, and
# every byte read back from 1 coming from 2), every one of them still holds —
# each address reaches exactly one environment, no stream commingles, and the
# interactions follow the address consistently. What distinguishes the two
# consoles is not how they behave but what they SAY they are, and only the
# environment itself can say it: at boot each POSIX sidecar writes its own
# index and pid onto its own console (user/sidecar/src/boot.rs, from the BIB's
# v3 identity words, which the kernel fills with the pid it gave that sidecar
# and the index it parsed out of that sidecar's own name). So this phase
# asserts the stream read at address 1 carries index 1's announcement and not
# index 2's, and symmetrically for 2.
#
# Note what it is NOT: it is not a read-back of the address that a "which
# environment is this" question would go to anyway. The announcement is emitted
# by the sidecar on its own output channel — the kernel files it under the
# entry whose channel it arrived on, not the entry a reader later looks up by
# env_id — so a kernel that swaps what an address MEANS cannot move the
# announcement with it. That asymmetry is the whole point: the claim travels
# by one path and is read by the other.
#
# Bounded: the announcement is written when the sidecar boots, which is around
# the same time as the manager's reply that binds the env_id, so a stream that
# is empty here is early rather than broken.
own1="[env-id] index=$i1 pid=$p1"
own2="[env-id] index=$i2 pid=$p2"
announced=0
for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
    drain_both
    case "$S1$S2" in *"[env-id]"*) announced=1; break ;; esac
    sleep 0.5
done
if [ "$announced" -ne 1 ]; then
    echo "FAILED: neither console ever announced its identity ('[env-id]' absent from both streams after ${ATTACH_WAIT_S}s) — the environment's own account of itself is not reaching its console" >&2
    fail=1
else
    case "$S1" in
        *"$own1"*) echo "ok:   identity: environment 1's stream announces its own index and pid ('$own1')" ;;
        *) echo "FAILED: identity: environment 1's console does not carry its own announcement (expected '$own1'; the '[env-id]' line it did carry: $(printf '%s' "$S1" | tr -d '\r' | grep -o '\[env-id\][^$]*' | head -1)) — the stream an operator reads at environment 1's address belongs to another environment" >&2; fail=1 ;;
    esac
    case "$S2" in
        *"$own2"*) echo "ok:   identity: environment 2's stream announces its own index and pid ('$own2')" ;;
        *) echo "FAILED: identity: environment 2's console does not carry its own announcement (expected '$own2'; the '[env-id]' line it did carry: $(printf '%s' "$S2" | tr -d '\r' | grep -o '\[env-id\][^$]*' | head -1))" >&2; fail=1 ;;
    esac
    case "$S1" in
        *"[env-id] index=$i2 pid=$p2"*) echo "FAILED: identity: environment 1's console carries environment 2's announcement ('$own2') — the two consoles are swapped" >&2; fail=1 ;;
        *) echo "ok:   identity: environment 1's stream carries no other environment's announcement" ;;
    esac
    case "$S2" in
        *"[env-id] index=$i1 pid=$p1"*) echo "FAILED: identity: environment 2's console carries environment 1's announcement ('$own1') — the two consoles are swapped" >&2; fail=1 ;;
        *) echo "ok:   identity: environment 2's stream carries no other environment's announcement" ;;
    esac
fi

console_send() {   # console_send <addr_env_id> <line> — waits for the peer to take it
    local id="$1" line="$2" t ok queued
    if [ "$TOOTH" = "no-input" ]; then
        echo "note: E6_TOOTH=no-input — withholding '$line'"
        return 0
    fi
    python3 - "$line" > "$W/send_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
    for t in $(seq 1 20); do
        api "$W/send.json" -X POST -d "$(cat "$W/send_body.json")" \
            "$BASE/api/partition/$pid/env/$id/console" || true
        ok="$(jval "$W/send.json" ok)"
        queued="$(jval "$W/send.json" queued)"
        if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then
            return 0
        fi
        if [ "$ok" = "false" ]; then
            echo "FAILED: writing to environment console $id was refused ('$(jval "$W/send.json" error)') — the attach surface's write half cannot reach it" >&2
            fail=1
            return 1
        fi
        # ok=true with queued=0 is the kernel's flow control: the peer has not
        # drained the previous line yet. Retried rather than treated as failure.
        sleep 1
    done
    echo "FAILED: environment console $id never accepted a line ('$line' stayed queued=0 for 20s — the environment is not reading its console)" >&2
    fail=1
    return 1
}

# ── Phase 6: the roadmap's command — the SAME text on both consoles ────────
# `setenv ENV <n>` is the shell's own builtin (applets.rs `sh`), so the variable
# lives in that environment and nowhere else. The command text is byte-identical
# for both, which is the point: a stream carrying `hi-2` at environment 1 cannot
# be explained by what the guard typed.
echo "note: same command text on both consoles — the marker can only come from the environment"
console_send "$(send_addr 1)" "setenv ENV 1"
console_send "$(send_addr 2)" "setenv ENV 2"
console_send "$(send_addr 1)" 'echo hi-$ENV | cat'
console_send "$(send_addr 2)" 'echo hi-$ENV | cat'
wait_for 1 "hi-1" "$ATTACH_WAIT_S" || true
wait_for 2 "hi-2" "$ATTACH_WAIT_S" || true

# isolate_check <label> <env1 marker> <env2 marker> — the four assertions, in
# one place because both marker families make exactly the same claim. Ordered
# own-output first: on a console that was never reachable, "no foreign bytes"
# is true and meaningless.
isolate_check() {
    local label="$1" m1="$2" m2="$3"
    local bad=0
    case "$S1" in
        *"$m1"*) echo "ok:   $label: environment 1's stream carries its own output ('$m1')" ;;
        *) echo "FAILED: $label: environment 1's console never delivered its own command's output ('$m1' absent after ${ATTACH_WAIT_S}s) — nothing ran on the environment this attach addressed" >&2; fail=1; bad=1 ;;
    esac
    case "$S2" in
        *"$m2"*) echo "ok:   $label: environment 2's stream carries its own output ('$m2')" ;;
        *) echo "FAILED: $label: environment 2's console never delivered its own command's output ('$m2' absent after ${ATTACH_WAIT_S}s) — nothing ran on the environment this attach addressed" >&2; fail=1; bad=1 ;;
    esac
    case "$S1" in
        *"$m2"*) echo "FAILED: $label: environment 1's console delivered environment 2's output ('$m2'): the two consoles are NOT isolated" >&2; fail=1; bad=1 ;;
        *) echo "ok:   $label: environment 1's stream carries none of environment 2's output" ;;
    esac
    case "$S2" in
        *"$m1"*) echo "FAILED: $label: environment 2's console delivered environment 1's output ('$m1'): the two consoles are NOT isolated" >&2; fail=1; bad=1 ;;
        *) echo "ok:   $label: environment 2's stream carries none of environment 1's output" ;;
    esac
    # The absence claims above are only meaningful while nothing was dropped:
    # env_console.c keeps the NEWEST 4 KiB and counts what fell off the front.
    if [ "${D1:-0}" = "0" ] && [ "${D2:-0}" = "0" ]; then
        echo "ok:   $label: nothing was dropped on either console (4 KiB buffers intact, so 'absent' means absent)"
    else
        echo "FAILED: $label: the kernel dropped bytes (env 1: ${D1:-?}, env 2: ${D2:-?}) — the streams above are not the whole story and isolation cannot be judged on them" >&2
        fail=1
        bad=1
    fi
    return "$bad"
}
isolate_check "phase A (echo hi-\$ENV | cat)" "hi-1" "hi-2"

# ── Phase 6b: the neighbour partition, held paused as the discriminator ───
# Assertion 6b in the header. Everything above is inside partition $pid: its
# two consoles are both ends of every claim the guard has made, identity
# announcement included. Two environments in DIFFERENT partitions can only be
# asked about with something outside the pair to compare against, and that
# something here is a second partition — with its own environment, deliberately
# at the SAME index (1), because the index is the environment's identity within
# its partition and not its address — which is then PAUSED. Pause is E4's own
# route (`POST /api/partition/pause` -> partition_pause(), kernel/partition.c):
# the partition keeps its processes, frames and consoles and is skipped by the
# scheduling rotation (kernel/process.c pick_next_partition), so its environment
# stops being scheduled.
#
# Why a paused peer is the discriminator, in one sentence: env_console_write()
# refuses a line while the peer's channel still holds the previous one, so a
# console whose far end cannot run accumulates an UNDRAINED queue — and an
# undrained queue is something only an environment that is not running can leave
# behind. So while the neighbour is frozen, a line addressed to it is accepted
# into its queue (queued > 0) and the line after it is refused (queued 0); if any
# other environment were serving that address, the queue would drain and that
# second line would be accepted. The property "one partition's environment is
# not reachable through another partition's console" thereby becomes an
# observable behaviour rather than a claim about a lookup table.
#
# The witness is built in the honest order — place, prove live, prove it runs
# its own commands — and only THEN frozen:
#   - the neighbour is created and reaches the attached state (live, index 1,
#     posix_pid distinct from $p1 and $p2) before anything is asserted about it,
#     so no clause below can pass vacuously on an environment that never ran;
#   - it announces its own identity on its own console and runs its own command
#     ('hi-nb-3', derived from a shell variable only that environment has);
#   - its partition is paused, and the pause is confirmed both over HTTP and in
#     the kernel's own words (Kernel TX, so a console race cannot lose it);
#   - with the neighbour frozen: its address still queues a line, the next line
#     is refused, partition $pid keeps working, and neither stream carries the
#     other's markers or the other's identity announcement;
#   - on resume the line queued during the pause is consumed by the NEIGHBOUR
#     ('hi-nb-9'), which is what makes the address the neighbour's rather than
#     merely unanswerable;
#   - and a (partition, env_id) pair that does not exist in that partition — the
#     neighbour's env_id read through partition $pid's address, and $E1 read
#     through the neighbour's — is refused on BOTH the read and the write half.
#     Those pairs are DERIVED, not assumed: the guard reads both listings and
#     establishes that each id is absent from the partition it is probed
#     through, since the manager's env_ids are globally monotonic
#     (user/init/src/env_manager.rs next_id) while the console registry is keyed
#     by (partition, env_id) (kernel/env_console.c ec_find_env).
#     A refusal family is also a family that can pass vacuously on a route which
#     refuses everything, so the same read is then made once on a pair that DOES
#     exist in its partition and has to answer — the non-vacuity control sits
#     next to the claim it protects rather than in another phase.
#
# Tooth: E6_TOOTH=skip-pause withholds the pause, which IS the discriminator;
# the neighbour then keeps draining, so the paused-peer clauses MUST fail and
# nothing else does. No kernel build needed, so it runs on every pass
# (tests/env_console_attach_check_smoke.sh).
#
# nsend_raw <line> — one POST to the NEIGHBOUR's address, no retry: a refusal is
# the expected outcome for some clauses here, so the caller has to see it as a
# verdict rather than have the retry loop explain it away. Sets SEND_OK /
# SEND_QUEUED.
# nsend <line> — the retrying form, for the neighbour while it is live and after
# its resume: a command that never lands is a failure, not something to wait out.
# ndrain — one read of the neighbour's console; S3 accumulates its bytes and D3
# its dropped counter (a read is destructive, like S1/S2, so this is the only
# place its stream is consumed).
nsend_raw() {   # nsend_raw <line> — one attempt at the neighbour's console
    local line="$1"
    python3 - "$line" > "$W/nsend_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
    api "$W/nsend.json" -X POST -d "$(cat "$W/nsend_body.json")" \
        "$BASE/api/partition/$npid/env/$NB/console" || true
    SEND_OK="$(jval "$W/nsend.json" ok)"
    SEND_QUEUED="$(jval "$W/nsend.json" queued)"
}
nsend() {   # nsend <line> — the neighbour's console, waiting for it to take it
    local t
    for t in $(seq 1 20); do
        nsend_raw "$1"
        if [ "$SEND_OK" = "true" ] && [ -n "$SEND_QUEUED" ] && [ "$SEND_QUEUED" != "0" ]; then
            return 0
        fi
        if [ "$SEND_OK" = "false" ]; then
            echo "FAILED: writing to the neighbour environment $NB's console was refused ('$(jval "$W/nsend.json" error)')" >&2
            fail=1
            return 1
        fi
        sleep 1
    done
    echo "FAILED: the neighbour environment $NB never accepted a line ('$1' stayed queued=0 for 20s — the environment is not reading its console)" >&2
    fail=1
    return 1
}
ndrain() {   # ndrain — one read of the neighbour's console into S3/D3
    drain_once "$npid" "$NB"
    if [ "$C_OK" = "true" ]; then
        S3="$S3$C_OUT"
        D3="$C_DROP"
    else
        echo "FAILED: the neighbour environment $NB's console is gone ('$(jval "$W/cons.json" error)') while this phase is asserting on it" >&2
        fail=1
    fi
}
if [ "$fail" -eq 0 ]; then
    S3=""; D3="0"
    npid=""
    npname="e6neighbour"
    npaused=0     # did THIS phase pause it (so its teardown can un-pause first)
    nresumed=0    # did the resume clause already run

    api "$W/npcreate.json" -X POST -d "{\"name\":\"$npname\"}" "$BASE/api/partitions" || true
    if [ "$(jval "$W/npcreate.json" ok)" = "true" ] && [ "$(jval "$W/npcreate.json" partition_id)" != "0" ]; then
        npid="$(jval "$W/npcreate.json" partition_id)"
        echo "ok:   a neighbour partition $npid ('$npname') exists — the witness outside this one"
    else
        echo "FAILED: could not define the neighbour partition (ok='$(jval "$W/npcreate.json" ok)' error='$(jval "$W/npcreate.json" error)')" >&2
        fail=1
    fi
fi

if [ "$fail" -eq 0 ] && [ -n "$npid" ]; then
    # Its environment, placed while the partition is still UNPAUSED — the kernel
    # refuses to charge a paused partition (E4's own gate), so the order is part
    # of the mechanism: the environment is placed, then the rotation is taken out
    # from underneath it. Index 1 on purpose: the same index as environment $E1.
    api "$W/nenv.json" -X POST -d '{"index":1}' "$BASE/api/partition/$npid/env" || true
    NB="$(jval "$W/nenv.json" env_id)"
    if [ "$(jval "$W/nenv.json" ok)" = "true" ] && [ -n "$NB" ] && [ "$NB" != "0" ]; then
        echo "ok:   the neighbour partition $npid has environment $NB (index 1 — the SAME index as environment $E1, a different partition)"
    else
        echo "FAILED: could not place an environment in the neighbour partition (env_id='$NB' ok='$(jval "$W/nenv.json" ok)' error='$(jval "$W/nenv.json" error)')" >&2
        fail=1
    fi
fi

# Its console, live and attributed — the same census shape as phase 5, for the
# other partition. Without this the phase below could pass vacuously on an
# environment that never came up (nothing to consume, nothing to leak).
if [ "$fail" -eq 0 ] && [ -n "$NB" ]; then
    nbound=0
    for i in $(seq 1 120); do
        api "$W/envsN.json" "$BASE/api/partition/$npid/env" || true
        if [ "$(jval "$W/envsN.json" live)" = "1" ] &&
           [ -n "$(env_field "$W/envsN.json" "$NB" index)" ]; then
            nbound=1
            break
        fi
        sleep 0.5
    done
    ni="$(env_field "$W/envsN.json" "$NB" index)"
    np_pid="$(env_field "$W/envsN.json" "$NB" posix_pid)"
    if [ "$nbound" = 1 ] && [ "$ni" = "1" ] && [ -n "$np_pid" ] && [ "$np_pid" != "$p1" ] && [ "$np_pid" != "$p2" ];
    then
        echo "ok:   the neighbour's console is live and attributed: env $NB -> index 1, posix pid $np_pid (distinct from $p1 and $p2)"
    else
        echo "FAILED: the neighbour's console never became live and attributed (live='$(jval "$W/envsN.json" live)', index='$ni', posix_pid='$np_pid' vs $p1/$p2)" >&2
        fail=1
    fi
    # Two sidecars with the SAME name in two partitions: the registry is keyed
    # by (partition, index) (E2), so both `aerosls.posix.1` are legitimately
    # registered and are different processes. That is exactly why a name- or
    # index-only address would be ambiguous and (partition, env_id) is not.
    processes_snapshot
    nn="$(proc_name_of "$W/procs.json" "$np_pid")"
    if [ "$nn" = "aerosls.posix.1" ] && [ "$n1" = "aerosls.posix.1" ] && [ "$np_pid" != "$p1" ]; then
        echo "ok:   partition $pid and partition $npid each have a sidecar NAMED 'aerosls.posix.1' (pids $p1 and $np_pid) — the name is not the address"
    else
        echo "FAILED: expected two distinct processes named 'aerosls.posix.1' in two partitions (partition $pid: '$n1' pid '$p1'; partition $npid: '$nn' pid '$np_pid')" >&2
        fail=1
    fi
fi

# The neighbour is a real environment: its own identity announcement, and its
# own command output, on its own address — before the pause below, so the pause
# is what changes its behaviour rather than its absence.
if [ "$fail" -eq 0 ] && [ -n "$NB" ]; then
    nown="[env-id] index=1 pid=$np_pid"
    for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
        ndrain
        case "$S3" in *"[env-id]"*) break ;; esac
        sleep 0.5
    done
    case "$S3" in
        *"$nown"*) echo "ok:   the neighbour announces its OWN identity ('$nown') — the same index as environment $E1's, its own partition and pid" ;;
        *) echo "FAILED: the neighbour's stream does not carry its own announcement (wanted '$nown')" >&2; fail=1 ;;
    esac

    # `setenv NB 3` then a marker derived from it: the OUTPUT text `hi-nb-3`
    # never appears in the typed line, so finding it is finding the neighbour's
    # own variable expansion (the same trick phase 6 uses for `$ENV`).
    nsend "setenv NB 3"
    nsend 'echo hi-nb-$NB | cat'
    for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
        ndrain
        case "$S3" in *"hi-nb-3"*) break ;; esac
        sleep 0.5
    done
    case "$S3" in
        *"hi-nb-3"*) echo "ok:   the neighbour runs its own commands on its own console ('hi-nb-3')" ;;
        *) echo "FAILED: the neighbour's console never delivered its own command's output ('hi-nb-3' absent after ${ATTACH_WAIT_S}s) — there is nothing to leak, so the pause below would prove nothing" >&2; fail=1 ;;
    esac
fi

# ── The pause: the discriminator itself ────────────────────────────────────
if [ "$fail" -eq 0 ] && [ -n "$NB" ]; then
    if [ "$TOOTH" = "skip-pause" ]; then
        echo "note: E6_TOOTH=skip-pause — NOT pausing partition $npid; the paused-neighbour clauses below MUST fail"
    else
        api "$W/npause.json" -X POST -d "{\"partition_id\":$npid}" "$BASE/api/partition/pause" || true
        if [ "$(jval "$W/npause.json" ok)" = "true" ]; then
            npaused=1
            echo "ok:   POST /api/partition/pause took the neighbour partition $npid out of the scheduling rotation"
        else
            echo "FAILED: could not pause the neighbour partition $npid (ok='$(jval "$W/npause.json" ok)')" >&2
            fail=1
        fi
        # The kernel's own words, independent of the HTTP body (kernel TX, so
        # not lost to a console race).
        if grep -aqF "[PARTITION] partition $npid paused -- excluded from scheduling rotation until resumed." "$LOG"; then
            echo "ok:   the kernel says so in its own words: partition $npid paused (excluded from the scheduling rotation)"
        else
            echo "FAILED: no '[PARTITION] partition $npid paused' line — the pause did not reach the scheduler" >&2
            fail=1
        fi
    fi

    # While the neighbour cannot run, a line addressed to it enters an empty
    # queue and STAYS there: env_console_write() refuses a write while the
    # peer's channel still holds the previous one, so the queue depth is what
    # says "nobody is draining this environment". The first line is retried
    # briefly — the last pre-pause line's consumption by the neighbour may still
    # be settling when the pause lands — and then has to be accepted.
    nfirst_ok=""; nfirst_queued=""
    for t in 1 2 3 4 5 6 7 8 9 10; do
        nsend_raw "setenv NB 9"
        nfirst_ok="$SEND_OK"; nfirst_queued="$SEND_QUEUED"
        if [ "$SEND_OK" = "true" ] && [ -n "$SEND_QUEUED" ] && [ "$SEND_QUEUED" != "0" ]; then
            break
        fi
        [ "$SEND_OK" = "false" ] && break
        sleep 1
    done
    if [ "$nfirst_ok" = "true" ] && [ -n "$nfirst_queued" ] && [ "$nfirst_queued" != "0" ]; then
        echo "ok:   a line addressed to the paused neighbour was accepted into its console queue (queued=$nfirst_queued) — the address is still its own even while it cannot run"
    else
        echo "FAILED: the paused neighbour's console did not accept a line (ok='$nfirst_ok', queued='$nfirst_queued' after 10s) — an address that cannot even queue is not an attachable address" >&2
        fail=1
    fi

    # The line that must NOT get through. With the neighbour frozen its queue
    # still holds the line above, so every attempt is refused (ok=true,
    # queued=0). Under E6_TOOTH=skip-pause the neighbour is live and drains that
    # queue, so an attempt IS accepted — which is how this phase tells "the pause
    # is real" from "the address is not the neighbour's". Several attempts, all
    # of which must be refused: one lucky refusal would not distinguish a paused
    # peer from a peer that momentarily had not drained yet.
    naccepted=0; nvanished=0; nattempts=""
    for t in 1 2 3 4 5; do
        nsend_raw 'echo hi-nb-$NB | cat'
        nattempts="$nattempts $SEND_OK/$SEND_QUEUED"
        if [ "$SEND_OK" = "false" ]; then nvanished=1; break; fi
        if [ "$SEND_QUEUED" != "0" ]; then naccepted=1; break; fi
        sleep 1
    done
    if [ "$nvanished" = 1 ]; then
        echo "FAILED: the paused neighbour's console vanished mid-phase ('$(jval "$W/nsend.json" error)')" >&2
        fail=1
    elif [ "$naccepted" = 0 ]; then
        echo "ok:   and every line after it was refused (ok/queued seen:$nattempts) because the paused peer had not drained the first — the queue belongs to an environment that is not running"
    else
        echo "FAILED: the paused neighbour DRAINED its queue (a later send was accepted: ok/queued seen:$nattempts) — something outside that partition is reading its console, or its partition is not out of the rotation" >&2
        fail=1
    fi

    # Meanwhile partition $pid keeps working, and the two streams stay each
    # their own. The neighbour's stream is drained again AFTER that work: a leak
    # would deliver partition $pid's marker (or the neighbour's own queued
    # command) across the partition boundary. A late-drained prompt is not a
    # leak, so the assertion is on marker content, not on "no bytes at all".
    console_send "$E1" 'echo hi-Aonly | cat'
    wait_for 1 "hi-Aonly" "$ATTACH_WAIT_S" || true
    ndrain
    case "$S1" in
        *"hi-Aonly"*) echo "ok:   partition $pid's environment kept working while its neighbour's partition was paused ('hi-Aonly')" ;;
        *) echo "FAILED: partition $pid's environment stopped answering while the NEIGHBOUR's partition was paused — the pause is not scoped to the neighbour" >&2; fail=1 ;;
    esac
    case "$S3" in
        *"hi-nb-9"*) echo "FAILED: a command sent while the neighbour's partition was paused ran and answered ('hi-nb-9' on its stream) — either the pause is not in effect, or something outside the neighbour's partition is draining its console" >&2; fail=1 ;;
        *"hi-Aonly"*) echo "FAILED: partition $pid's output appeared on the NEIGHBOUR's console ('hi-Aonly') — the two partitions' addresses are crossed" >&2; fail=1 ;;
        *) echo "ok:   while paused the neighbour's stream carries nothing that ran elsewhere (no 'hi-nb-9', no 'hi-Aonly')" ;;
    esac
    case "$S1" in
        *"hi-nb"*) echo "FAILED: partition $pid's stream carries the neighbour's command or output ('hi-nb...') — the addresses are crossed" >&2; fail=1 ;;
        *) echo "ok:   partition $pid's stream carries nothing the neighbour typed or produced" ;;
    esac
fi

# ── Resume: the paused line was the neighbour's, and only the neighbour's ──
if [ "$fail" -eq 0 ] && [ -n "$NB" ] && [ "$TOOTH" != "skip-pause" ]; then
    api "$W/nresume.json" -X POST -d "{\"partition_id\":$npid}" "$BASE/api/partition/resume" || true
    if [ "$(jval "$W/nresume.json" ok)" = "true" ]; then
        nresumed=1
        echo "ok:   the neighbour's partition is resumed"
    else
        echo "FAILED: could not resume the neighbour partition $npid (ok='$(jval "$W/nresume.json" ok)')" >&2
        fail=1
    fi
    # The line queued during the pause is consumed by the NEIGHBOUR: it sets NB
    # again, so the marker below can only come from the queued `setenv NB 9`
    # having reached the neighbour and nobody else.
    nsend 'echo hi-nb-$NB | cat'
    for i in $(seq 1 $((ATTACH_WAIT_S * 2))); do
        ndrain
        case "$S3" in *"hi-nb-9"*) break ;; esac
        sleep 0.5
    done
    case "$S3" in
        *"hi-nb-9"*) echo "ok:   the line queued while the neighbour was paused was consumed by the NEIGHBOUR after resume ('hi-nb-9') — the address was the neighbour's all along" ;;
        *) echo "FAILED: the neighbour never consumed the line queued during its pause ('hi-nb-9' absent after ${ATTACH_WAIT_S}s) — the line went somewhere else, or nowhere" >&2; fail=1 ;;
    esac
    case "$S1" in
        *"hi-nb-9"*) echo "FAILED: the line queued for the neighbour produced its output on partition $pid's stream ('hi-nb-9') — an address in one partition reached the other's environment" >&2; fail=1 ;;
        *) echo "ok:   and partition $pid's stream never carried it" ;;
    esac
fi

# env_ids <envs.json> — the env_ids in one partition's console listing
env_ids() {
    python3 - "$1" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
print(" ".join(str(e.get("env_id")) for e in d.get("envs", [])))
PY
}

# ── Neither partition is reachable through the other's address ────────────
# The direct form of the property this phase exists for: a (partition, env_id)
# pair that does not exist in that partition is not an address. Both halves are
# probed, because the write half is what delivers input INTO an environment and
# the read half is what carries its output out.
#
# The pairs are DERIVED, not assumed. The guard reads both listings and probes
# an id only after seeing it absent from the partition it is read through: the
# manager's env_ids are globally monotonic (next_id in
# user/init/src/env_manager.rs) while the console registry is keyed by
# (partition, env_id) (env_console.c ec_find_env), so the neighbour's id is not
# an id of partition $pid and $E1 is not an id of the neighbour's — but that is
# what this run has to OBSERVE, not a belief the guard is allowed to lean on.
if [ "$fail" -eq 0 ] && [ -n "$NB" ]; then
    envs_snapshot
    api "$W/envsN.json" "$BASE/api/partition/$npid/env" || true
    ids_pid=" $(env_ids "$W/envs.json") "
    ids_npid=" $(env_ids "$W/envsN.json") "
    for pair in "$pid $NB" "$npid $E1"; do
        xp="${pair%% *}"; xid="${pair##* }"
        if [ "$xp" = "$pid" ]; then xhere="$ids_pid"; else xhere="$ids_npid"; fi
        case "$xhere" in
            *" $xid "*)
                echo "FAILED: env_id $xid IS listed in partition $xp (listing:'$xhere'), so this run cannot probe a cross-partition address with it — env_ids are not scoped the way this phase assumes" >&2
                fail=1
                continue ;;
        esac
        echo "note: partition $xp's listing is '$xhere' — env $xid is not one of its environments, which is what the next two requests probe"
        api "$W/xread.json" "$BASE/api/partition/$xp/env/$xid/console" || true
        if [ "$(jval "$W/xread.json" ok)" = "false" ] &&
           grep -qF "no such environment console in this partition" "$W/xread.json"; then
            echo "ok:   reading environment $xid through partition $xp's address is refused ('no such environment console in this partition')"
        else
            echo "FAILED: GET /api/partition/$xp/env/$xid/console answered ok='$(jval "$W/xread.json" ok)' — env $xid is not an environment of partition $xp and must not be reachable through its address (body: $(head -c 200 "$W/xread.json"))" >&2
            fail=1
        fi
        api "$W/xsend.json" -X POST -d '{"input":"echo hi-cross"}' \
            "$BASE/api/partition/$xp/env/$xid/console" || true
        if [ "$(jval "$W/xsend.json" ok)" = "false" ] &&
           grep -qF "no such environment console in this partition" "$W/xsend.json"; then
            echo "ok:   writing to environment $xid through partition $xp's address is refused too"
        else
            echo "FAILED: POST /api/partition/$xp/env/$xid/console answered ok='$(jval "$W/xsend.json" ok)' (queued='$(jval "$W/xsend.json" queued)') — input must not be deliverable across partitions' addresses" >&2
            fail=1
        fi
    done

    # Non-vacuity, proved next to the claim rather than inferred from elsewhere:
    # a route that answered "no such environment console" to everything would
    # satisfy every refusal above while being useless. So the SAME read is made
    # once on a pair that DOES exist in its partition, and it has to answer.
    # (This drains environment $E1's console, which is harmless here: every
    # assertion made from those bytes has already been made, and the phases below
    # assert on commands they send themselves.)
    api "$W/xvalid.json" "$BASE/api/partition/$pid/env/$E1/console" || true
    if [ "$(jval "$W/xvalid.json" ok)" = "true" ] && [ "$(jval "$W/xvalid.json" env_id)" = "$E1" ]; then
        echo "ok:   and the same read on (partition $pid, env $E1) — a pair that DOES exist in its partition — answers ok=true, so the refusals above are about the pair and not about the route"
    else
        echo "FAILED: GET /api/partition/$pid/env/$E1/console did not answer ok=true for a pair that exists in its partition (ok='$(jval "$W/xvalid.json" ok)' env_id='$(jval "$W/xvalid.json" env_id)') — the cross-partition refusals above cannot be judged against a route that never answers" >&2
        fail=1
    fi

    # The behavioural half of the same claim. The refusals above are the
    # control-plane answer; a write that was refused and delivered anyway would
    # show up as the marker the refused line asked for, on whichever of the
    # three consoles ends up serving the environment that received it. All three
    # are read here, and read for long enough that a delivered command would have
    # run (the environment has to be scheduled before it can answer).
    for i in $(seq 1 $((ATTACH_WAIT_S / 3 + 1))); do
        drain_both
        ndrain
        case "$S1$S2$S3" in *"hi-cross"*) break ;; esac
        sleep 0.5
    done
    case "$S1$S2$S3" in
        *"hi-cross"*) echo "FAILED: 'hi-cross' appeared in a stream — a write the control plane refused as a cross-partition address was delivered to an environment anyway (the address matched an environment outside the partition it named)" >&2; fail=1 ;;
        *) echo "ok:   and none of the three streams carries 'hi-cross' (${ATTACH_WAIT_S}s) — the refused writes reached no environment at all" ;;
    esac
fi

# ── The neighbour's teardown, and this phase's negative clauses ───────────
# Every claim above is an absence as much as a presence, so the two ways an
# absence can be hollow are closed here: bytes dropped on the neighbour's
# console (its "not in my stream" would then be a memory hole, not isolation),
# and any of partition $pid's markers or its environments' identity
# announcements appearing on the neighbour's side, or the neighbour's on
# partition $pid's. This runs whenever the neighbour exists — including on a run
# that has already gone red — so a tooth arm does not leave the guest holding a
# partition it created, and it un-pauses first if an earlier clause skipped the
# resume. The `${NB:-}` forms are load-bearing under `set -u`: if the neighbour's
# placement itself failed there is nothing to tear down, and that has to read as
# one clause going red rather than as the guard dying on an unbound variable.
if [ -n "${NB:-}" ]; then
    ndrain
    case "$S3" in
        *"hi-1"*|*"hi-2"*|*"hi-env1"*|*"hi-env2"*|*"hi-Aonly"*)
            echo "FAILED: the neighbour partition's console carries an environment of partition $pid's output — the two partitions' consoles are crossed" >&2; fail=1 ;;
        *) echo "ok:   across the whole phase the neighbour's console carried none of partition $pid's markers" ;;
    esac
    case "$S1$S2" in
        *"hi-nb"*)
            echo "FAILED: partition $pid's streams carry the neighbour's commands or output ('hi-nb...') — the addresses are crossed" >&2; fail=1 ;;
        *) echo "ok:   across the whole phase partition $pid's streams carried nothing the neighbour typed or produced" ;;
    esac
    if [ -n "${np_pid:-}" ]; then
        case "$S1$S2" in
            *"pid=$np_pid"*)
                echo "FAILED: partition $pid's streams carry the NEIGHBOUR's identity announcement (pid $np_pid) — an address in one partition is showing the other's environment" >&2; fail=1 ;;
            *) echo "ok:   and neither of partition $pid's streams carries the neighbour's identity announcement" ;;
        esac
    fi
    if [ "${D3:-0}" = "0" ]; then
        echo "ok:   nothing was dropped on the neighbour's console either (${D3:-0}) — its absent markers are absent, not fallen off a full buffer"
    else
        echo "FAILED: the neighbour's console dropped bytes (${D3:-?}) — its stream is not the whole story and its absence claims cannot be judged on it" >&2
        fail=1
    fi
    if [ "${npaused:-0}" = 1 ] && [ "${nresumed:-0}" != 1 ]; then
        api "$W/nresume.json" -X POST -d "{\"partition_id\":$npid}" "$BASE/api/partition/resume" || true
        echo "note: the neighbour's partition was resumed on the way out (an earlier clause skipped the resume; the destroy below would otherwise run against a partition out of the rotation)"
    fi
    api "$W/ndestroy.json" -X POST -d "{\"env_id\":$NB}" "$BASE/api/partition/$npid/env/destroy" || true
    if [ "$(jval "$W/ndestroy.json" ok)" = "true" ]; then
        echo "ok:   the neighbour environment $NB destroyed over the E5 route"
    else
        echo "FAILED: could not destroy the neighbour environment $NB (error='$(jval "$W/ndestroy.json" error)')" >&2
        fail=1
    fi
    ngone=0
    for i in $(seq 1 $((DESTROY_WAIT_S * 2))); do
        api "$W/envsN.json" "$BASE/api/partition/$npid/env" || true
        if [ "$(jval "$W/envsN.json" live)" = "0" ]; then
            ngone=1
            break
        fi
        sleep 0.5
    done
    if [ "$ngone" = 1 ]; then
        echo "ok:   the neighbour's console left the kernel registry with it (partition $npid live=0)"
    else
        echo "FAILED: the neighbour's console lingered after its environment was destroyed (partition $npid live='$(jval "$W/envsN.json" live)')" >&2
        fail=1
    fi
fi

# ── Phase 8: destroying one environment does not disturb the other ────────
# Phase 6 catches OUTPUT halves crossed (the marker that appears is not the one
# this environment produced) and input halves crossed (the marker family
# moves). This phase pins the input direction with text that names its
# destination, so a stream that received the wrong bytes says so in its own
# content.
if [ "$fail" -eq 0 ]; then
    console_send "$(send_addr 1)" 'echo hi-env1 | cat'
    console_send "$(send_addr 2)" 'echo hi-env2 | cat'
    wait_for 1 "hi-env1" "$ATTACH_WAIT_S" || true
    wait_for 2 "hi-env2" "$ATTACH_WAIT_S" || true
    isolate_check "phase B (echo hi-envN | cat)" "hi-env1" "hi-env2"
else
    echo "note: phase B skipped — a clause above went red, so this phase's own results would be read as if they were that clause's"
fi

# ── Phase 8: destroying one environment does not disturb the other ────────
# Two claims, and the second is why the phase is here rather than in the E5
# guard: the destroyed environment's console must LEAVE (env_console.h: "a
# destroyed environment's console cannot linger"), and the survivor must still
# be attached to and still be isolated from the corpse's bytes. The E5 guard
# proves the lifecycle; this proves the console registry follows it.
if [ "$fail" -eq 0 ]; then
    api "$W/destroy1.json" -X POST -d "{\"env_id\":$E1}" "$BASE/api/partition/$pid/env/destroy" || true
    if [ "$(jval "$W/destroy1.json" ok)" = "true" ]; then
        echo "ok:   environment $E1 destroyed over the E5 route"
    else
        echo "FAILED: could not destroy environment $E1 (ok='$(jval "$W/destroy1.json" ok)' error='$(jval "$W/destroy1.json" error)')" >&2
        fail=1
    fi
    left=0
    for i in $(seq 1 $((DESTROY_WAIT_S * 2))); do
        envs_snapshot
        if [ "$(jval "$W/envs.json" live)" = "1" ] &&
           [ -z "$(env_field "$W/envs.json" "$E1" index)" ]; then
            left=1
            break
        fi
        sleep 0.5
    done
    if [ "$left" = 1 ]; then
        echo "ok:   the destroyed environment's console left the kernel registry (live=1, env $E1 gone; env $E2 still there)"
    else
        echo "FAILED: the destroyed environment's console lingered (after ${DESTROY_WAIT_S}s: live='$(jval "$W/envs.json" live)', env $E1 index='$(env_field "$W/envs.json" "$E1" index)') — a destroyed environment's console must not stay attachable" >&2
        fail=1
    fi
    read -r cok _dup <<EOF
$(console_get "$pid" "$E1")
EOF
    if [ "$cok" = "false" ]; then
        echo "ok:   attaching to the destroyed environment answers 'no such environment console in this partition'"
    else
        echo "FAILED: GET on the destroyed environment's console answered ok='$cok' — attach still reaches an environment that no longer exists" >&2
        fail=1
    fi
    if grep -aq "\[ENV-CONSOLE\] environment (partition $pid, index 1) ended with" "$LOG"; then
        echo "note: the kernel retired it on the peer close ($(grep -a "\[ENV-CONSOLE\] environment (partition $pid, index 1) ended" "$LOG" | tail -1 | tr -d '\r'))"
    fi
    # The survivor still runs commands, and the corpse's bytes are not in it.
    console_send "$E2" 'echo hi-env2b | cat'
    wait_for 2 "hi-env2b" "$ATTACH_WAIT_S" || true
    case "$S2" in
        *"hi-env2b"*) echo "ok:   environment $E2 is still attachable after its neighbour was destroyed (ran a fresh command)" ;;
        *) echo "FAILED: environment $E2 stopped answering its console after environment $E1 was destroyed" >&2; fail=1 ;;
    esac
    case "$S2" in
        *"hi-1"*) echo "FAILED: environment $E2's stream carries environment $E1's output ('hi-1')" >&2; fail=1 ;;
        *) echo "ok:   environment $E2's stream carries nothing environment $E1 produced" ;;
    esac
    api "$W/destroy2.json" -X POST -d "{\"env_id\":$E2}" "$BASE/api/partition/$pid/env/destroy" || true
    if [ "$(jval "$W/destroy2.json" ok)" = "true" ]; then
        echo "ok:   environment $E2 destroyed too (the guard leaves no environment behind)"
    else
        echo "FAILED: could not destroy environment $E2 (error='$(jval "$W/destroy2.json" error)')" >&2
        fail=1
    fi
    cleared=0
    for i in $(seq 1 $((DESTROY_WAIT_S * 2))); do
        envs_snapshot
        if [ "$(jval "$W/envs.json" live)" = "0" ]; then
            cleared=1
            break
        fi
        sleep 0.5
    done
    if [ "$cleared" = 1 ]; then
        echo "ok:   both consoles are gone from the registry after both destroys (live=0)"
    else
        echo "FAILED: a console survived its environment (live='$(jval "$W/envs.json" live)' after both destroys)" >&2
        fail=1
    fi
fi

# ── Verdict ────────────────────────────────────────────────────────────────
echo
if [ "$fail" -ne 0 ]; then
    report_and_exit
fi
echo "PASS  E6 attach: two environments in partition $pid, each reachable through its own console, neither stream carrying a byte the other produced"
exit 0

#!/usr/bin/env bash
# tests/e7_gate_check.sh — the E7 gate's check harness.
#
# ─── What one invocation owns ──────────────────────────────────────────────
# `--live` is the whole gate run: build the gate image (the DRILL build by
# default, see below), boot it cold, create the designated gate environment
# through the environment manager, let the designated first user
# (`backup/aerobackup`) run its real nine-entry command line until the kernel
# holds it at the copy continuation, CHECKPOINT, power the machine off, boot
# the SAME NVMe image again, replay the record through POST /api/env/restore,
# and assert the gate's facts on the second boot's serial transcript:
#
#   * the restore reports payloads=1, payload_skips=0 (the contents came back,
#     they were not refused by name);
#   * the Linux tenant was ADOPTED at a captured context (`form=3`,
#     `stop=spent`) and the copy RESUMED at the captured source offset — its
#     FIRST post-restore read is `off=0x30000`, and there is no second
#     `Copying`, which would mean it restarted;
#   * exactly one `[backup] Done:` line, no `[FAULT]`, and the tenant's exit
#     tears the address space down (`frame(s) freed`).
#
# ─── The drill is PART of the gate check, not a side script ────────────────
# The pour's wait used to bound ELAPSED time: a sidecar still serving the
# replay's own boot I/O when a fixed ~30 s deadline expired was reported as a
# posture that never came, and a healthy environment read as one whose
# contents were refused (run 12 of increment 21). The wait now bounds QUIET
# (EP_WAIT_TICKS restarted by any switch-in on the record's sidecar tasks or
# the `[env-id]` announcement, EP_WAIT_CEIL_TICKS as the absolute stop), and
# the only way to exercise that live is to make the flake to order: the
# `e7_slow_boot` sidecar feature runs a compute burst in the gate profile's
# own boot so the OLD bound lands on a sidecar that is RUNNING. That build is
# what this harness compiles by default (E7G_DRILL=1), and its assertions are
# gate clauses: the wait must outlive the fixed bound with a sidecar running,
# must never answer (`wait ended` absent), and the pour must still land. The
# feature is compiled out of every shipped image (E7G_DRILL=0 builds the plain
# gate image and the drill clauses become inapplicable).
#
# ─── The TEETH are part of it too (increment 22) ───────────────────────────
# A gate clause is only worth its failure side. `--teeth` captures ONE
# checkpoint, then replays it FOUR times against sparse copies of the same
# NVMe image: the control (no tooth, must be green) and the three breaks
# kernel/env_payload.h documents — `descriptor` (the payload's Linux task
# record dropped: no adoption, the child restarts), `no-fd` (the
# continuation's read forwarded as a descriptor the sidecar's table does not
# carry: EBADF, no offset line, no Done) and `no-heap` (the reply's
# destination page unmapped before the forward: the read IS issued at the
# captured offset, the deliver refuses its bytes -EFAULT, and the tenant's own
# free of that 64 KiB buffer then faults INSIDE the withheld page — so B3
# stays green while B4/B5 redden, which is how the harness tells "the read
# never happened" from "the page was gone"). Each tooth declares the clauses it
# MUST redden and the ones it MUST leave green; a mismatch in either
# direction is a failure, so the teeth cannot pass by breaking everything.
# The teeth image is a separate build (`E7_TOOTH=1`, a different ISO name) and
# every other build refuses a named tooth by name, so no shipped kernel can be
# asked to break an environment. A replay whose pour is REFUSED (the P1a
# posture window: a sidecar still running when the wait's ceiling expires)
# never applied its tooth, so that boot is retried on a fresh copy, visibly;
# a pour that refuses every attempt fails the run.
#
# ─── Modes ─────────────────────────────────────────────────────────────────
#   (no arguments)     the SOURCE clauses over this tree only (no build, no QEMU)
#   --live             the source clauses, then the build + the two-boot run
#   --teeth            the source clauses, then the teeth build + ONE capture
#                      + four replays (control + the three breaks)
#   --replay DIR       validate a recorded run's artifacts only (no build, no
#                      QEMU) — the same assertions, over boot2.log + facts;
#                      a recording made with a tooth is validated under that
#                      tooth's expectation (it is self-describing)
#   positional ROOT    the source clauses over another tree (the hermetic seam)
#
# ─── Knobs (all optional; the defaults are the gate check) ─────────────────
#   E7G_DRILL=1        build the sidecar with `e7_slow_boot` and assert the
#                      drill clauses (0 = the plain gate image, drill clauses
#                      recorded as not-applicable). --teeth defaults it to 0:
#                      the breaks are the run's subject, not the wait's.
#   E7G_BUILD=1        0 reuses E7G_ISO as-is (the caller built it)
#   E7G_ISO            default sls_operating_system_e3_drill.iso for --live
#                      and sls_operating_system_e3_teeth.iso for --teeth — both
#                      dedicated names, so a run never replaces the plain gate
#                      image with a build carrying a test-only profile
#   E7G_BOOT_ENTRY     default 3 (the unified grub entry)
#   E7G_PORT           default tests/free_port.sh
#   E7G_ARTIFACTS      where boot1.log/boot2.log/facts/JSON land
#                      (default /tmp/e7-gate-$$, printed at the end)
#   E7G_WORKLOAD_S     seconds to wait for the resumed workload (default 900)
#
# ─── What this does NOT claim ──────────────────────────────────────────────
# It does not run in CI: the source clauses do (they need only bash), but
# --live and --teeth need make + QEMU and take ~15 and ~40 minutes on this host
# (the 512 KiB firstuser build is the slow part; the four replays are ~3). The
# teeth cover the three breaks increment 22 names (descriptor, fd, heap page);
# the restart and stale-`fs_base` teeth the design also lists are NOT here —
# the restart is what `descriptor` produces as a consequence and is asserted as
# B3/B4, and `fs_base` has no arm yet.
#
# GUARD-KIND: host (the source clauses need only bash; the live/teeth arms add
# make + QEMU and are the only modes that build anything).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIVE=0
TEETH=0
REPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --live)   LIVE=1; shift ;;
        --teeth)  TEETH=1; shift ;;
        --replay) REPLAY="${2:-}"; shift 2 ;;
        -*)       echo "ABORT: unknown argument '$1' (usage: $0 [ROOT] [--live|--teeth] [--replay DIR])" >&2; exit 2 ;;
        *)        ROOT="$1"; shift ;;
    esac
done
cd "$ROOT" || exit 2

fails=0
red_count=0
ok()  { echo "ok:   $1"; }
red() { echo "red:  $1"; red_count=$((red_count + 1)); }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
has() { grep -qF -- "$2" "$ROOT/$1" 2>/dev/null; }
line_of() { grep -nF -- "$2" "$ROOT/$1" 2>/dev/null | head -1 | cut -d: -f1; }

# ─── A. Preconditions ───────────────────────────────────────────────────────
for f in Makefile kernel/env_payload.c kernel/env_payload.h kernel/process.c \
         kernel/syscall_dispatch.c user/sidecar/Cargo.toml user/sidecar/src/entry.rs \
         user/sidecar/src/e7_firstuser.rs kernel/env_service.c net/http.c; do
    if [ ! -f "$ROOT/$f" ]; then
        echo "ABORT: $f is missing — this is not the AeroSLS tree the gate lives in" >&2
        exit 2
    fi
done

# ─── G. The wiring the run depends on ──────────────────────────────────────
# Every clause is one fact whose absence would make the live run pass for the
# wrong reason, or fail a 15-minute boot for a reason a grep could name now.

# G1. The drill is a BUILD-TIME profile, compiled only into the drill build,
#     and it lives in the gate branch of the sidecar's boot — before the
#     tenant is spawned, or the bound would land after the boot it is meant to
#     catch.
drill_missing=""
has user/sidecar/Cargo.toml "e7_slow_boot = []" || drill_missing="$drill_missing feature"
has Makefile "--features aerosls-sidecar/e7_slow_boot" || drill_missing="$drill_missing makefile-knob"
has Makefile "E7_SLOW_BOOT" || drill_missing="$drill_missing knob-var"
has user/sidecar/src/entry.rs 'gate_boot_stretch(&RealKernel, caps.console_chan);' || drill_missing="$drill_missing call"
has user/sidecar/src/entry.rs '#[cfg(feature = "e7_slow_boot")]' || drill_missing="$drill_missing cfg"
if [ -n "$drill_missing" ]; then
    bad "G1. the drill build is not wired:$drill_missing — the gate check's drill arm would run a plain image and its 'outlived the fixed bound' clause would fail for the build's reason, not the wait's"
else
    call_line=$(line_of user/sidecar/src/entry.rs 'gate_boot_stretch(&RealKernel, caps.console_chan);')
    stage_line=$(line_of user/sidecar/src/entry.rs 'crate::e7_firstuser::stage_gate(&mut booted.proc.vfs, 0)')
    spawn_line=$(line_of user/sidecar/src/entry.rs 'crate::e7_firstuser::spawn_gate(')
    if [ -n "${stage_line:-}" ] && [ -n "${spawn_line:-}" ] && \
       [ "${call_line:-0}" -gt "$stage_line" ] && [ "${call_line:-0}" -lt "$spawn_line" ]; then
        ok "G1. the drill is a build-time feature (sidecar Cargo + Makefile knob) whose stretch runs in the gate profile between staging (line $stage_line) and the tenant spawn (line $spawn_line)"
    else
        bad "G1. the drill stretch (line ${call_line:-?}) does not sit between the staging (line ${stage_line:-?}) and the spawn (line ${spawn_line:-?}) — a stretch anywhere else does not make the replay's boot the thing the bound lands on"
    fi
fi

# G2. The wait's contract: TWO bounds, a stall window restarted by progress and
#     an absolute ceiling, and the progress signal is the switch-in ledger the
#     scheduler keeps (not a guess the wait makes).
if ! has kernel/env_payload.h "EP_WAIT_TICKS" || \
   ! has kernel/env_payload.h "EP_WAIT_CEIL_TICKS (EP_WAIT_TICKS * 6u)"; then
    bad "G2. the wait's two bounds are not declared in env_payload.h — the drill's 'outlived the fixed bound' assertion names EP_WAIT_TICKS, and a wait with one bound cannot extend"
elif ! has kernel/env_payload.c "pd->switches != last_switch[t]" || \
     ! has kernel/process.c "next->switches++"; then
    bad "G2. the wait does not sample the switch-in ledger, or no switch path bumps it — 'still being scheduled' would be an inference, not a fact"
else
    ok "G2. the wait is bounded by EP_WAIT_TICKS (stall, restarted by progress) and EP_WAIT_CEIL_TICKS (absolute), and its progress signal is ProcessDescriptor.switches, bumped at proc_build_resume_frame"
fi

# G3. The diagnostic the drill reads: the line that separates an EXTENDING
#     wait from an EXPIRED one, printed once and only while the window is
#     still open, and it must name the posture — running vs parked is the
#     whole difference between the failure this fixes and a pass for the
#     wrong reason.
if ! has kernel/env_payload.c "outlived the fixed bound" || \
   ! has kernel/env_payload.c "running=%u/%u"; then
    bad "G3. the wait's outlived-the-bound line (with the running posture) is missing — the drill's assertions and the transcript have nothing to read"
elif ! has kernel/env_payload.c "kernel_tick_counter < stall_deadline"; then
    bad "G3. the outlived line is not gated on the window still being open — a stalled wait would print 'extended' beside its own 'stalled' verdict"
else
    ok "G3. the wait reports crossing the old fixed bound once, only while the window is still open (progress restarted it), and names how many of the record's sidecars are RUNNING at that instant"
fi

# G4. The gate tenant itself: a 9-entry designated command line (argv[0]
#     included), the 512 KiB staged image with its 256 KiB marker, and the
#     continuation the checkpoint is taken at.
if ! has user/sidecar/src/e7_firstuser.rs "GATE_ARGV: [&str; 9]" || \
   ! has user/sidecar/src/e7_firstuser.rs "GATE_IMAGE_BYTES: u64 = 512 * 1024" || \
   ! has user/sidecar/src/e7_firstuser.rs "GATE_MARKER_OFFSET: u64 = 256 * 1024"; then
    bad "G4. the gate tenant is not the designated one (9-entry argv, 512 KiB staged image, marker at 256 KiB) — the run would measure a different command line"
elif ! has kernel/process.h "PROC_GATE_SRC_OFF 196608" || \
     ! has kernel/env_payload.c "PROC_GATE_SRC_OFF"; then
    bad "G4. the copy continuation (PROC_GATE_SRC_OFF 196608) is not the stop the pour spends — the resume offset this harness asserts would be unrelated to the capture"
else
    ok "G4. the gate tenant is the designated 9-entry command line over the 512 KiB staged image, and the checkpoint's stop is the copy continuation at 196608"
fi

# G5. What the run reads and what orders the pour: the wait sits before the
#     repause, the pour after it (a yield between them is the straight-line
#     rule's whole point), and the report carries both counters by name.
if ! has kernel/env_service.c "(void)env_payload_wait_parked(&w);" || \
   ! has kernel/env_service.c "env_payload_restore(done[d].p, done[d].idx,"; then
    bad "G5. the restore pass does not wait-then-pour — the run's payloads/payload_skips counters would describe a different order"
elif [ -z "$(line_of kernel/env_service.c '(void)env_payload_wait_parked(&w);')" ] || \
     [ -z "$(line_of kernel/env_service.c 'out->n_repaused  = env_ckpt_restore_repause();')" ]; then
    bad "G5. the pre-pour wait or the repause is not where this clause expects it — the ordering assertion cannot be evaluated"
elif [ "$(line_of kernel/env_service.c '(void)env_payload_wait_parked(&w);')" -ge \
       "$(line_of kernel/env_service.c 'out->n_repaused  = env_ckpt_restore_repause();')" ]; then
    bad "G5. the pre-pour wait is not before the repause — yielding after the snapshot's pause state is back risks resuming Ring-3 into memory the pour overwrites"
elif ! has net/http.c 'jb_uint(&j,"payloads", rep.n_payloads);' || \
     ! has net/http.c 'jb_uint(&j,"payload_skips", rep.n_payload_skips);'; then
    bad "G5. the restore report does not carry payloads/payload_skips — this harness's central assertion has no route"
else
    ok "G5. the restore pass waits before the repause and pours after it, and the route reports payloads and payload_skips by name"
fi

# G6. The teeth: a build-time profile, selected per restore, compiled out of
#     every other build — and the three arms land on paths the shipped code
#     already refuses by name (a name with no process, a fd the table does not
#     carry, an unmapped destination), never on a fault of their own.
teeth_missing=""
has kernel/env_payload.h "#define E7G_TOOTH 0" || teeth_missing="$teeth_missing default-off"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_DESCRIPTOR 1' || teeth_missing="$teeth_missing descriptor-code"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_NO_FD      2' || teeth_missing="$teeth_missing no-fd-code"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_NO_HEAP    3' || teeth_missing="$teeth_missing no-heap-code"
has Makefile 'E7_TOOTH ?=' || teeth_missing="$teeth_missing makefile-knob"
has Makefile 'AB_DEFS += -DE7G_TOOTH=1' || teeth_missing="$teeth_missing makefile-define"
has kernel/env_payload.c '[E7-GATE] tooth descriptor:' || teeth_missing="$teeth_missing descriptor-arm"
has kernel/env_payload.c 'ep_skip[pi] = 1;' || teeth_missing="$teeth_missing descriptor-pages"
has kernel/env_payload.c 'pd->tooth = (uint8_t)((ep_tooth == ENV_PAYLOAD_TOOTH_NO_FD' || teeth_missing="$teeth_missing descriptor-stamp"
has kernel/syscall_dispatch.c '[E7-GATE] tooth no-fd:' || teeth_missing="$teeth_missing no-fd-arm"
has kernel/syscall_dispatch.c 'largs[0] = ENV_PAYLOAD_TOOTH_DEAD_FD;' || teeth_missing="$teeth_missing no-fd-substitute"
has kernel/syscall_dispatch.c '[E7-GATE] tooth no-heap:' || teeth_missing="$teeth_missing no-heap-arm"
has kernel/syscall_dispatch.c 'user_unmap_page(cur->cr3, pv);' || teeth_missing="$teeth_missing no-heap-unmap"
has net/http.c 'env_payload_build_has_teeth()' || teeth_missing="$teeth_missing route-gate"
has net/http.c 'this image was not built with the gate teeth' || teeth_missing="$teeth_missing route-refusal"
has net/http.c 'jb_str(&j,"tooth", env_payload_tooth_name(tooth));' || teeth_missing="$teeth_missing route-report"
has kernel/env_service.c 'env_payload_set_tooth(es_restore_tooth);' || teeth_missing="$teeth_missing pass-selection"
if [ -n "$teeth_missing" ]; then
    bad "G6. the gate teeth are not wired:$teeth_missing — --teeth would either run the control four times or break an environment outside a teeth build"
else
    shipped=$(grep -c 'E7G_TOOTH' kernel/env_payload.c kernel/syscall_dispatch.c 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')
    if ! has kernel/env_payload.h "#ifndef E7G_TOOTH"; then
        bad "G6. E7G_TOOTH has no default — a build that never passes the define would not compile the arms out deterministically"
    else
        ok "G6. the teeth are a build-time profile (E7_TOOTH -> AB_DEFS, default off) selected per restore and refused by name elsewhere; all three arms land on the shipped refusals (dropped record, EBADF lookup, unmapped destination), guarded by $shipped E7G_TOOTH conditions"
    fi
fi

# ─── The recorded-run validator (--replay, and the live arm's own last step) ─
# One set of assertions over artifacts, so a live run, a replayed run and a
# tooth run cannot drift: each arm writes the files, then calls this exact
# code. A probe returns 0 (green) or 1 (red) and leaves its sentence in CM;
# whether that state is a PASS depends on the tooth the recording names.
fact() { sed -n "s/^$2=//p" "$1/facts" 2>/dev/null | tail -1; }
CM=""

p_b1() {   # the payload landed
    local d="$1" payloads skips
    payloads=$(fact "$d" payloads); skips=$(fact "$d" payload_skips)
    if [ "$payloads" = "1" ] && [ "$skips" = "0" ]; then
        CM="the replay landed the payload (payloads=1, payload_skips=0) — the contents came back, they were not withheld or refused by name"
        return 0
    fi
    CM="the replay did not land the payload (payloads=${payloads:-?}, payload_skips=${skips:-?}) — the environment came back as an empty replay, which is exactly the failure the gate exists to detect"
    return 1
}

p_b2() {   # the Linux tenant was adopted at a captured context
    local d="$1" b2="$d/boot2.log"
    if grep -aqF "[ENV_PAYLOAD] linux task 'e7.gate.user.0' adopted at" "$b2" && \
       grep -aq "form=3 .*stop=spent" "$b2"; then
        CM="the Linux tenant was adopted from the captured context (form=3, stop=spent) — not restarted at _start, and the one-shot stop was spent by the pour"
        return 0
    fi
    CM="no adopted-context line for the tenant (form=3, stop=spent) — the replay either lost the task or left its stop armed"
    return 1
}

p_b3() {   # the FIRST post-restore read is the captured continuation
    local d="$1" b2="$d/boot2.log" first
    first=$(grep -aoE '\[E7-GATE\] read off=[0-9A-Fa-f]+' "$b2" 2>/dev/null \
                | head -1 | sed 's/.*off=//' | tr 'a-f' 'A-F')
    if [ "$first" = "00030000" ] || [ "$first" = "30000" ]; then
        CM="the resumed copy's FIRST read is at the captured source offset (off=0x30000) — the task continued from the stop, it did not re-enter the copy"
        return 0
    fi
    if [ -z "$first" ]; then
        CM="no read reached the file class after the restore — the resumed task never issued the continuation (the forward was refused before it, or the task never ran)"
    else
        CM="the first read after the restore is at off=0x$first, not the captured 0x30000 — the copy resumed at the beginning of the file, not at the stop"
    fi
    return 1
}

p_b4() {   # exactly the resumed workload, once
    local d="$1" b2="$d/boot2.log" copying done
    copying=$(grep -acF "[backup] Copying" "$b2")
    done=$(grep -acF "[backup] Done:" "$b2")
    if [ "$copying" -eq 0 ] && [ "$done" -ge 1 ]; then
        CM="exactly the resumed workload completed: no second [backup] Copying, at least one [backup] Done:"
        return 0
    fi
    CM="the workload did not resume cleanly (Copying=${copying}, Done=${done}) — a second Copying is a restart, and no Done is an unfinished gate"
    return 1
}

p_b5() {   # no fault
    local d="$1" faults
    faults=$(grep -acF "[FAULT]" "$d/boot2.log")
    if [ "$faults" -eq 0 ]; then
        CM="no [FAULT] in the replay boot"
        return 0
    fi
    CM="$faults [FAULT] line(s) in the replay boot — the resumed tenant (or its sidecars) faulted"
    return 1
}

p_b6() {   # the tenant's frames came back
    local d="$1"
    if grep -aq "frame(s) freed" "$d/boot2.log"; then
        CM="the tenant's exit tore its address space down (frame(s) freed) — the resumed descriptors and heap were returned, not leaked"
        return 0
    fi
    CM="no address-space teardown after the tenant's exit — the resumed environment leaked its frames"
    return 1
}

p_d1() {   # the drill: the wait outlived the fixed bound with a sidecar running
    local d="$1" b2="$d/boot2.log"
    if ! grep -aqF "[E7-GATE] stretch-done" "$b2"; then
        CM="the drill build's boot stretch never finished in the replay — this image was not built with E7_SLOW_BOOT=1, so the drill clauses would be measuring a plain gate run"
        return 1
    fi
    if ! grep -aqF "outlived the fixed bound" "$b2"; then
        CM="the wait never passed the old fixed bound — the stretch did not outlast EP_WAIT_TICKS, so no extension was exercised"
        return 1
    fi
    if ! grep -aqE "outlived the fixed bound.*running=[12]/2" "$b2"; then
        CM="the wait crossed the bound with no sidecar RUNNING — this run exercised a parked posture, not the one the old deadline refused: $(grep -aF 'outlived the fixed bound' "$b2" | tail -1)"
        return 1
    fi
    CM="the wait outlived the old fixed bound with a sidecar RUNNING ($(grep -aoE 'running=[12]/2' "$b2" | head -1)) — the exact posture a fixed elapsed bound reported as a posture that never came"
    return 0
}

p_d2() {   # the wait never answered
    local d="$1"
    if grep -aqF "wait ended" "$d/boot2.log"; then
        CM="the wait answered anyway: $(grep -aF 'wait ended' "$d/boot2.log" | tail -1) — the stall window must not end on an environment it has seen being scheduled"
        return 1
    fi
    CM="the wait never answered (no 'wait ended' line) and the pour still landed — the extension is the hardening, not a wider timeout that still refuses"
    return 0
}

# The clauses each tooth MUST redden. Everything else must stay green: a tooth
# that reddens outside its set proves nothing about the clause it names, and
# one that leaves its set green proves the clause cannot see the break.
expected_red() {
    case "$1" in
        ""|none)    echo "" ;;
        descriptor) echo "B2 B3 B4" ;;
        no-fd)      echo "B3 B4" ;;        no-heap)   echo "B4 B5" ;;

        *)          echo "?" ;;
    esac
}

# The named evidence: the transcript must carry the break's own line, and for
# the two forward teeth the tenant's own error must be the errno the break
# produces — not merely "it failed".
tooth_evidence() {   # <dir> <tooth> -> 0 and CM, or 1 and CM
    local d="$1" t="$2" b2="$1/boot2.log"
    case "$t" in
        descriptor)
            if ! grep -aqF "[E7-GATE] tooth descriptor:" "$b2"; then
                CM="the dropped-record line ('[E7-GATE] tooth descriptor: ... DROPPED') is missing — the recording cannot say which break produced these reds"
                return 1
            fi
            CM="the kernel's own line names the break ('[E7-GATE] tooth descriptor: ... is DROPPED'), and the reds above are its consequences"
            return 0 ;;
        no-fd)
            if ! grep -aqF "[E7-GATE] tooth no-fd:" "$b2"; then
                CM="the substituted-fd line ('[E7-GATE] tooth no-fd: ...') is missing — the recording cannot say which break produced these reds"
                return 1
            fi
            # The tenant's error is printed through a console descriptor, and
            # the kernel traces every write, so the SENTENCE is fragmented
            # across lines (`Bad file descriptor`, ` (os error `, `9`, `)`).
            # The errno's NAME is a single write and is what is stable; that
            # it is EBADF and not another errno is the whole assertion.
            if ! grep -aqF "Bad file descriptor" "$b2"; then
                CM="the tenant did not answer EBADF — a break that lands anywhere else is not the fd break this tooth claims"
                return 1
            fi
            CM="the forward names the substituted descriptor and the tenant's own error names EBADF ('Bad file descriptor')"
            return 0 ;;
        no-heap)
            if ! grep -aqF "[E7-GATE] tooth no-heap:" "$b2"; then
                CM="the unmapped-destination line ('[E7-GATE] tooth no-heap: ...') is missing — the recording cannot say which break produced these reds"
                return 1
            fi
            # The deliver refuses the bytes (-EFAULT), and the tenant's own
            # unwinding then reads the hole: its 64 KiB Vec's chunk header sits
            # at the page's last bytes, so `free` faults AT THE WITHHELD PAGE.
            # That address, not a generic crash, is what this clause checks —
            # a fault anywhere else would not be this break.
            local page addr
            page=$(grep -aoE 'tooth no-heap: .*page 0x[0-9a-fA-F]+' "$b2" | head -1 | sed 's/.*0x//')
            addr=$(grep -aoE 'Ring-3 #PF .*addr=0x[0-9a-fA-F]+' "$b2" | head -1 | sed 's/.*0x//')
            if [ -z "$page" ] || [ -z "$addr" ]; then
                CM="the break is not visible where it must land: tooth page='${page:-?}' fault addr='${addr:-?}' — a fault anywhere but the withheld page is not the heap-page break"
                return 1
            fi
            if [ $((0x$addr & ~0xFFF)) -ne $((0x$page)) ]; then
                CM="the [FAULT] is at 0x$addr, outside the withheld page 0x$page — the red is not this tooth's"
                return 1
            fi
            CM="the forward names the withheld page 0x$page and the tenant's free of its 64 KiB buffer faults inside it (addr=0x$addr) — the deliver refused the bytes, the hole is the tenant's own"
            return 0 ;;
        *)
            CM="the recording names an unknown tooth ('$t') — it cannot be validated against an expectation"
            return 1 ;;
    esac
}

# One clause: run its probe, compare its state with the tooth's expectation,
# and print `ok`, `red` (a tooth's expected red, counted as a pass) or FAIL.
report_clause() {   # <dir> <clause> <probe> <red-set>
    local d="$1" cl="$2" probe="$3" want="$4" state want_red=0
    "p_$probe" "$d"; state=$?
    case " $want " in *" $cl "*) want_red=1 ;; esac
    if [ "$state" -eq 0 ]; then
        if [ "$want_red" -eq 1 ]; then
            bad "$cl. GREEN where this tooth must redden it — the clause does not measure what it claims: $CM"
        else
            ok "$cl. $CM"
        fi
    else
        if [ "$want_red" -eq 1 ]; then
            red "$cl. $CM"
        else
            bad "$cl. $CM"
        fi
    fi
}

assert_artifacts() {
    local d="$1"
    local b2="$d/boot2.log"
    if [ ! -f "$b2" ] || [ ! -f "$d/facts" ]; then
        bad "the recording is incomplete: $b2 or $d/facts is missing"
        return
    fi
    local tooth drill want
    tooth=$(fact "$d" tooth); [ -n "$tooth" ] || tooth="none"
    drill=$(fact "$d" drill)
    want=$(expected_red "$tooth")
    if [ "$want" = "?" ]; then
        bad "the recording names tooth '$tooth', which this harness has no expectation for — refusing to judge it"
        return
    fi
    if [ "$tooth" != "none" ]; then
        echo "--- tooth '$tooth': this recording is EXPECTED to redden [$want] and to leave every other clause green"
    fi

    report_clause "$d" B1 b1 "$want"
    report_clause "$d" B2 b2 "$want"
    report_clause "$d" B3 b3 "$want"
    report_clause "$d" B4 b4 "$want"
    report_clause "$d" B5 b5 "$want"
    report_clause "$d" B6 b6 "$want"

    if [ "$drill" = "1" ]; then
        report_clause "$d" D1 d1 "$want"
        report_clause "$d" D2 d2 "$want"
    else
        ok "D1. the drill clauses are not applicable: the recording says drill=0 (the plain gate image)"
    fi

    if [ "$tooth" != "none" ]; then
        if tooth_evidence "$d" "$tooth"; then
            ok "T. $CM"
        else
            bad "T. $CM"
        fi
    fi
}

if [ -n "$REPLAY" ]; then
    if [ ! -d "$REPLAY" ]; then
        echo "ABORT: --replay dir '$REPLAY' is not a directory" >&2
        exit 2
    fi
    assert_artifacts "$REPLAY"
    if [ "$fails" -ne 0 ]; then
        echo "e7_gate_check: $fails clause(s) failed on the recorded run in $REPLAY"
        exit 1
    fi
    if [ "$red_count" -gt 0 ]; then
        echo "e7_gate_check: the recorded run in $REPLAY is a tooth run — $red_count clause(s) reddened exactly where its tooth says they must, and it cannot be read as a passing gate"
        exit 0
    fi
    echo "e7_gate_check: the recorded run in $REPLAY passes the gate's clauses"
    exit 0
fi

if [ "$LIVE" -eq 0 ] && [ "$TEETH" -eq 0 ]; then
    if [ "$fails" -ne 0 ]; then
        echo "e7_gate_check: $fails clause(s) failed"
        exit 1
    fi
    echo "e7_gate_check: the gate's source wiring holds (use --live for the run, --teeth for the breaks)"
    exit 0
fi

# ─── The live arms ─────────────────────────────────────────────────────────
if [ "$LIVE" -eq 1 ]; then
    DRILL="${E7G_DRILL:-1}"
    ISO="${E7G_ISO:-sls_operating_system_e3_drill.iso}"
else
    DRILL="${E7G_DRILL:-0}"
    ISO="${E7G_ISO:-sls_operating_system_e3_teeth.iso}"
fi
# The teeth are a BUILD, not a request: a named tooth on a non-teeth kernel is
# refused by the route, so the run must build the arms in.
TOOTH_BUILD="${E7G_TOOTH:-$TEETH}"
BUILD="${E7G_BUILD:-1}"
ENTRY="${E7G_BOOT_ENTRY:-3}"
WINDOW_S="${E7G_WORKLOAD_S:-900}"
ART="${E7G_ARTIFACTS:-/tmp/e7-gate-$$}"
TOKEN=deadbeef01234567cafebabe76543210
IMG="$ART/nvme.img"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
WD="$ART/work"

mkdir -p "$ART" "$WD" || { echo "ABORT: cannot create $ART" >&2; exit 2; }

QPID=""
CATPID=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${CATPID:-}" ] && kill "$CATPID" 2>/dev/null
    return 0
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; cleanup; exit 1; }

if [ "$DRILL" != "0" ] && [ "$DRILL" != "1" ]; then
    fail "E7G_DRILL must be 0 or 1 (got '$DRILL')"
fi
if [ "$TOOTH_BUILD" != "0" ] && [ "$TOOTH_BUILD" != "1" ]; then
    fail "E7G_TOOTH must be 0 or 1 (got '$TOOTH_BUILD')"
fi
if [ "$BUILD" = "1" ]; then
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT=$DRILL E7_TOOTH=$TOOTH_BUILD X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT="$DRILL" \
        E7_TOOTH="$TOOTH_BUILD" X86_E3_ISO="$ISO" || fail "the gate image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (E7G_BUILD=0 needs a prebuilt one)"

boot() {   # boot <log> <tag> -> sets QPID, PORT
    local log="$1" tag="$2"
    local ser="$ART/ser_${tag}"
    PORT="${E7G_PORT:-}"
    [ -n "$PORT" ] || PORT=$(bash tests/free_port.sh) || exit 2
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
        -serial pipe:"$ser" 2>"$ART/${tag}_qerr.txt" &
    QPID=$!
    bash tests/grub_select_kernel_only.sh "$ser.in" "$log" "$QPID" "$ENTRY" || \
        fail "grub entry $ENTRY did not boot ($tag)"
    local ready=1 i
    for i in $(seq 1 480); do
        if grep -aq "unified=1" "$log" 2>/dev/null && \
           grep -aq "Listening on port 3000" "$log" 2>/dev/null && \
           [ "$(grep -acF '[INIT] heartbeat' "$log")" -ge 5 ]; then
            ready=0; break
        fi
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the control plane was up ($tag)"
        sleep 0.5
    done
    [ "$ready" -eq 0 ] || fail "the unified boot did not reach its markers ($tag)"
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
power_off() {
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    kill "$CATPID" 2>/dev/null
    QPID=""; CATPID=""
}

# Boot 1: create the designated environment, run the workload to the copy
# continuation, checkpoint, power off. Sets PID. Shared by --live and --teeth
# so the two arms capture the SAME thing (one capture, four replays, is what
# makes the teeth a comparison rather than four unrelated runs).
gate_capture() {
    echo "=== boot 1: create, workload to the stop, checkpoint ==="
    boot "$L1" one
    BASE="http://127.0.0.1:$PORT"

    api "$WD/pcreate.json" -X POST -d '{"name":"e7gate"}' "$BASE/api/partitions" || fail "POST /api/partitions"
    PID=$(jval "$WD/pcreate.json" partition_id)
    [ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
    echo "partition=$PID"

    api "$WD/create.json" -X POST -d '{"index":1}' "$BASE/api/partition/$PID/env" || fail "POST env"
    echo "create: $(cat "$WD/create.json")"

    local spawned=0 held=0 i
    for i in $(seq 1 240); do
        grep -aqF "[E7] firstuser: spawned" "$L1" && { spawned=1; break; }
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the tenant spawned"
        sleep 0.5
    done
    [ "$spawned" -eq 1 ] || fail "the gate tenant never spawned"

    # The checkpoint is armed at the tenant's OWN continuation (the fourth
    # 64 KiB read, src_off=196608), which the kernel holds before forwarding;
    # waiting for that line is what makes the captured posture deterministic
    # instead of a race with the copy.
    for i in $(seq 1 480); do
        grep -aqF "[E7-GATE] held at the copy continuation" "$L1" && { held=1; break; }
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the tenant reached its stop"
        sleep 0.5
    done
    [ "$held" -eq 1 ] || fail "the tenant never reached the copy continuation"
    grep -aF "[E7-GATE] held" "$L1" | tail -1
    echo "--- reads before the stop:"
    grep -aF "[E7-GATE] read off=" "$L1" | tail -5

    api "$WD/checkpoint.json" -X POST "$BASE/api/checkpoint" || fail "POST /api/checkpoint"
    echo "checkpoint: $(cat "$WD/checkpoint.json")"
    grep -aqF "captured partition=$PID index=1" "$L1" || fail "the capture line never appeared"
    grep -aF "[ENV_PAYLOAD] capture:" "$L1" | tail -4

    echo "=== power off (the checkpoint is on the NVMe image) ==="
    power_off
}

# Boot 2: adopt + replay, with the restore body given (P1a's `payload` knob
# and, in a teeth build, `tooth`). Waits for the workload's own conclusion —
# the tenant's exit tears its address space down, or a fault — because a
# tooth's failure is only over when the task is reaped, and the replayed
# console bytes can echo an old `Done:` before the run has done anything.
gate_replay() {   # gate_replay <log> <tag> <body> <artifact-dir>
    local log="$1" tag="$2" body="$3" ard="$4"
    # The serial log IS the recording's boot2.log, and boot() opens it for
    # writing before anything else — so its directory has to exist first.
    mkdir -p "$ard" "$(dirname "$log")" || fail "cannot create $ard"
    echo "=== boot 2 ($tag): adopt + replay${body:+ body=$body} ==="
    boot "$log" "$tag"
    BASE="http://127.0.0.1:$PORT"

    echo "--- adopted records:"
    grep -aE "ENV_CKPT|adopt" "$log" | head -10

    api "$ard/restore.json" -X POST -d "$body" "$BASE/api/env/restore" || \
        fail "POST /api/env/restore did not answer (tail: $(tail -c 400 "$log"))"
    echo "restore: $(cat "$ard/restore.json")"
    case "$(cat "$ard/restore.json")" in
        *'"ok":"true"'*) : ;;
        *) fail "the restore route refused the request: $(cat "$ard/restore.json")" ;;
    esac

    local i
    for i in $(seq 1 $((WINDOW_S * 2))); do
        grep -aq "frame(s) freed" "$log" && break
        grep -aqF "[FAULT]" "$log" && break
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the workload concluded ($tag)"
        sleep 0.5
    done
    sleep 2
    power_off

    echo "--- restore lines:"
    grep -aF "[ENV_RESTORE]" "$log" | tail -6
    echo "--- payload lines:"
    grep -aF "[ENV_PAYLOAD]" "$log" | tail -6
    echo "--- tenant lines (boot 2):"
    grep -aE "\[backup\]|\[E7\] linux|\[FAULT\]" "$log" | tail -12
    echo "--- copies (boot 2): $(grep -acF '[backup] Copying' "$log")  done (boot 2): $(grep -acF '[backup] Done:' "$log")  faults (boot 2): $(grep -acF '[FAULT]' "$log")"

    {
        echo "drill=$DRILL"
        echo "iso=$ISO"
        echo "payloads=$(jval "$ard/restore.json" payloads)"
        echo "payload_skips=$(jval "$ard/restore.json" payload_skips)"
        echo "tooth=$(jval "$ard/restore.json" tooth)"
        echo "partition=$PID"
    } > "$ard/facts"
    [ -f "$ard/boot2.log" ] || fail "boot 2's serial log was not recorded"
}

if [ "$LIVE" -eq 1 ]; then
    rm -f "$IMG"
    truncate -s 10G "$IMG" || fail "could not make the NVMe image"
    gate_capture
    L2="$ART/boot2.log"
    gate_replay "$L2" two '{}' "$ART"

    echo "=== gate clauses (the recording is reusable with --replay $ART) ==="
    assert_artifacts "$ART"

    if [ "$fails" -ne 0 ]; then
        echo "e7_gate_check: $fails clause(s) failed; artifacts in $ART"
        exit 1
    fi
    echo "e7_gate_check: the gate run passes — artifacts in $ART"
    exit 0
fi

# ─── The teeth arm ─────────────────────────────────────────────────────────
# One capture, then four replays over SPARSE COPIES of the post-checkpoint
# image: the control must be fully green (the arms do not fire unselected) and
# each tooth must redden exactly its declared set, with its own line in the
# transcript. The copy is `--sparse=always` so the 10 GiB image costs its
# allocated extents, not ten gigabytes multiplied by five.
rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"
gate_capture

echo "=== the teeth: one capture, four replays ==="
TC="$ART/teeth"
mkdir -p "$TC" || fail "cannot create $TC"
# The capture is the SOURCE for every copy: a replay must not be asked to
# carry the next tooth's checkpoint, and the record surviving a replay is a
# property of the record layer, not something this loop may lean on.
SRC_IMG="$IMG"
retries=0

# A replay whose pour was REFUSED never applied its tooth: all three breaks
# live in the pour or in the forward path after it. The refusal seen here is
# the P1a posture window (a sidecar still RUNNING when the wait's ceiling
# expires — env_payload.h's account of run 12), not a tooth's red, so this arm
# retries that boot ON A FRESH COPY and prints the attempt. The retry is
# bounded and visible; a replay that never gets its contents injected can
# never be counted as a pass for any tooth, and a pour that refuses EVERY
# attempt fails the run instead of being retried away.
replay_for_tooth() {   # <name> <tag-base> <body> -> 0 when the pour landed
    local name="$1" tag="$2" body="$3" try payloads skips
    for try in 1 2 3; do
        [ "$try" -gt 1 ] && retries=$((retries + 1))
        cp --sparse=always "$SRC_IMG" "$TC/$name.img" || \
            fail "could not copy the checkpointed image for $name"
        IMG="$TC/$name.img"
        rm -rf "$TC/$name"
        gate_replay "$TC/$name/boot2.log" "${tag}r$try" "$body" "$TC/$name"
        payloads=$(fact "$TC/$name" payloads)
        skips=$(fact "$TC/$name" payload_skips)
        if [ "$payloads" = "1" ]; then return 0; fi
        echo "note: $name attempt $try got no pour (payloads=${payloads:-?}, payload_skips=${skips:-?}) — the P1a posture window, not this tooth; retrying on a fresh copy"
    done
    return 1
}

i=0
for tooth in control descriptor no-fd no-heap; do
    i=$((i + 1))
    case "$tooth" in
        control)   body='{}' ;;
        *)         body="{\"tooth\":\"$tooth\"}" ;;
    esac
    replay_for_tooth "$tooth" "t$i" "$body" || \
        fail "$tooth: no attempt got the payload poured — the run cannot measure this tooth (artifacts in $ART)"
done
IMG="$SRC_IMG"

echo "=== teeth clauses (each recording is reusable with --replay) ==="
for tooth in control descriptor no-fd no-heap; do
    echo "--- $tooth"
    assert_artifacts "$TC/$tooth"
done

if [ "$fails" -ne 0 ]; then
    echo "e7_gate_check: $fails clause(s) failed; artifacts in $ART"
    exit 1
fi
if [ "$retries" -gt 0 ]; then
    echo "e7_gate_check: $retries replay(s) were retried for the P1a posture window (a pour refusal, not a tooth's red); the recordings kept are the ones that poured"
fi
echo "e7_gate_check: the teeth redden by name — $red_count clause(s) red across descriptor/no-fd/no-heap, each exactly where its tooth says, the control green; artifacts in $ART"
exit 0

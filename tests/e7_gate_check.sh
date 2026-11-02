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
# and assert the gate's facts across the two boots' serial transcripts:
#
#   * the restore reports payloads=1, payload_skips=0 (the contents came back,
#     they were not refused by name);
#   * the Linux tenant was ADOPTED at a captured context (`form=3`,
#     `stop=spent`) at the capture's own `fs_base`, and the copy RESUMED at
#     the captured source offset — its
#     FIRST post-restore read is `off=0x30000`, and there is no second
#     `Copying`, which would mean it restarted;
#   * exactly one `[backup] Done:` line, no `[FAULT]`, and the tenant's exit
#     tears the address space down (`frame(s) freed`);
#   * the stack was MEASURED: the work half's stop line and the resumed
#     tenant's exit line carry the same poisoned span, their own headroom and
#     deepest agree, and the headroom clears the gate's floor.
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
# checkpoint, then replays it FIVE times against sparse copies of the same
# NVMe image: the control (no tooth, must be green) and the four breaks
# kernel/env_payload.h documents — `descriptor` (the payload's Linux task
# record dropped: no adoption, the child restarts), `no-fd` (the
# continuation's read forwarded as a descriptor the sidecar's table does not
# carry: EBADF, no offset line, no Done), `no-heap` (the reply's
# destination page unmapped before the forward: the read IS issued at the
# captured offset, the deliver refuses its bytes -EFAULT, and the tenant's own
# free of that 64 KiB buffer then faults INSIDE the withheld page — so B3
# stays green while B4/B5 redden, which is how the harness tells "the read
# never happened" from "the page was gone"), and `fs_base` (the adopted
# context's TLS base re-stamped one page off the capture's, into a page the
# record itself says is mapped writable — adoption happens and the
# continuation read is issued, then musl dereferences a pointer read from the
# wrong TCB and the tenant dies: B2 red by name, B4/B5 as the stale base's
# consequence; the complement of `descriptor`, where the tenant never adopts
# and B3 restarts from zero). Each tooth declares the clauses it
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
#                      + five replays (control + the four breaks)
#   --selfkill         the source clauses, then the self-kill build + ONE boot:
#                      the gate tenant asks for its own death at the stop while
#                      RUNNING, the deferred teardown scans its stack, and no
#                      checkpoint is taken
#   --replay DIR       validate a recorded run's artifacts only (no build, no
#                      QEMU) — the same assertions, over boot1.log, boot2.log
#                      and facts;
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
#   E7G_ISO            default sls_operating_system_e3_drill.iso for --live,
#                      sls_operating_system_e3_teeth.iso for --teeth and
#                      sls_operating_system_e3_selfkill.iso for --selfkill —
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
# (the 512 KiB firstuser build is the slow part; the five replays are ~4). The
# teeth cover all four breaks increment 22/23 name (descriptor, fd, heap page,
# TLS base); the restart tooth is not a separate arm — it is what `descriptor`
# produces as a consequence and is asserted as B3/B4. B7's exit scan rides BOTH teardown
# paths — process_kill for a normal kill/exit and process_exit for a task that
# dies on the fault path — so the `no-heap` recording carries an exit line too,
# and an adopted tenant that produces none reddens the clause by name.
#
# GUARD-KIND: host (the source clauses need only bash; the live/teeth arms add
# make + QEMU and are the only modes that build anything).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIVE=0
TEETH=0
SELF=0
REPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --live)     LIVE=1; shift ;;
        --teeth)    TEETH=1; shift ;;
        --selfkill) SELF=1; shift ;;
        --replay)   REPLAY="${2:-}"; shift 2 ;;
        -*)         echo "ABORT: unknown argument '$1' (usage: $0 [ROOT] [--live|--teeth|--selfkill] [--replay DIR])" >&2; exit 2 ;;
        *)          ROOT="$1"; shift ;;
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
#     every other build — and the four arms land on paths the shipped code
#     already has (a name with no process, a fd the table does not carry, an
#     unmapped destination, a TLS base moved only to a page the record says is
#     mapped writable), never on a fault of their own.
teeth_missing=""
has kernel/env_payload.h "#define E7G_TOOTH 0" || teeth_missing="$teeth_missing default-off"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_DESCRIPTOR 1' || teeth_missing="$teeth_missing descriptor-code"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_NO_FD      2' || teeth_missing="$teeth_missing no-fd-code"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_NO_HEAP    3' || teeth_missing="$teeth_missing no-heap-code"
has kernel/env_payload.h 'ENV_PAYLOAD_TOOTH_FS_BASE    4' || teeth_missing="$teeth_missing fs-base-code"
has Makefile 'E7_TOOTH ?=' || teeth_missing="$teeth_missing makefile-knob"
has Makefile 'AB_DEFS += -DE7G_TOOTH=1' || teeth_missing="$teeth_missing makefile-define"
has kernel/env_payload.c '[E7-GATE] tooth descriptor:' || teeth_missing="$teeth_missing descriptor-arm"
has kernel/env_payload.c 'ep_skip[pi] = 1;' || teeth_missing="$teeth_missing descriptor-pages"
has kernel/env_payload.c 'pd->tooth = (uint8_t)((ep_tooth == ENV_PAYLOAD_TOOTH_NO_FD' || teeth_missing="$teeth_missing descriptor-stamp"
has kernel/syscall_dispatch.c '[E7-GATE] tooth no-fd:' || teeth_missing="$teeth_missing no-fd-arm"
has kernel/syscall_dispatch.c 'largs[0] = ENV_PAYLOAD_TOOTH_DEAD_FD;' || teeth_missing="$teeth_missing no-fd-substitute"
has kernel/syscall_dispatch.c '[E7-GATE] tooth no-heap:' || teeth_missing="$teeth_missing no-heap-arm"
has kernel/syscall_dispatch.c 'user_unmap_page(cur->cr3, pv);' || teeth_missing="$teeth_missing no-heap-unmap"
has kernel/env_payload.c '[E7-GATE] tooth fs_base:' || teeth_missing="$teeth_missing fs-base-arm"
has kernel/env_payload.c 'ep_page_recorded_writable(block, np' || teeth_missing="$teeth_missing fs-base-choice"
has kernel/env_payload.c 'pd->fs_base        = adopted_fs;' || teeth_missing="$teeth_missing fs-base-install"
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
        ok "G6. the teeth are a build-time profile (E7_TOOTH -> AB_DEFS, default off) selected per restore and refused by name elsewhere; all four arms land on paths the shipped kernel already has (dropped record, EBADF lookup, unmapped destination, a TLS base chosen from the record's own writable pages), guarded by $shipped E7G_TOOTH conditions"
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

p_b2() {   # the Linux tenant was adopted at a captured context, TLS base included
    local d="$1" b1="$d/boot1.log" b2="$d/boot2.log"
    local ad cap_fs ad_fs
    ad=$(grep -aF "[ENV_PAYLOAD] linux task 'e7.gate.user.0' adopted at" "$b2" 2>/dev/null | tail -1)
    cap_fs=$(grep -aF "capture: task" "$b1" 2>/dev/null | grep -aF "'e7.gate.user.0'" | \
                 tail -1 | sed -n 's/.*fs_base=0x\([0-9a-fA-F]*\).*/\1/p')
    ad_fs=$(printf '%s\n' "$ad" | sed -n 's/.*fs_base=0x\([0-9a-fA-F]*\).*/\1/p')
    if [ -n "$ad" ] && printf '%s\n' "$ad" | grep -q "form=3 .*stop=spent"; then
        if [ -z "$cap_fs" ] || [ -z "$ad_fs" ]; then
            CM="the tenant was adopted but the TLS base cannot be compared: the capture names fs_base='${cap_fs:-none}', the adoption '${ad_fs:-none}' — a recording from before the capture line carried it cannot show which base came back"
            return 1
        fi
        if [ "$ad_fs" = "$cap_fs" ]; then
            CM="the Linux tenant was adopted from the captured context (form=3, stop=spent) at the capture's fs_base=0x$cap_fs — the TLS base came back with it, and the copy was not restarted"
            return 0
        fi
        CM="the tenant was adopted (form=3, stop=spent) but at fs_base=0x$ad_fs, not the capture's 0x$cap_fs — the context carried one TLS base and installed another"
        return 1
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

# ─── Q2: the stack's high-water measurement, checked instead of quoted ─────
# The gate profile poisons the span below the entry %rsp at create, and the
# kernel reports it twice: at the stop (boot 1's work half) and at the resumed
# tenant's teardown (boot 2), on whichever teardown path the tenant leaves by.
# The two paths are exclusive, so the clause also counts the exit lines and
# refuses a doubled one. A line is only evidence if its numbers agree, so the
# clause re-derives them: the budget is the gate's 64 KiB, the poisoned
# span is inside it, headroom + deepest is that span, the lowest touched
# address sits exactly `headroom` past a page-aligned base, and the headroom
# clears a floor. The floor is half the region: the measured run leaves 57,424
# bytes (87.6%), and a workload that eats into the last 32 KiB is the
# regression this clause exists to catch, not run-to-run noise. A budget
# change is a deliberate edit to HWM_ALLOC and the design's number, not a
# silent drift a log line can absorb.
HWM_ALLOC=65536
HWM_MIN_HEADROOM=32768

hwm_field() {   # <line> <name> -> its decimal value, empty when absent
    printf '%s\n' "$1" | grep -oE "$2=[0-9]+" | head -1 | cut -d= -f2
}

hwm_parse() {   # <line> <where> -> 0 with HWM_* set, 1 and CM otherwise
    local line="$1" where="$2"
    HWM_ALLOC_V=$(hwm_field "$line" alloc)
    HWM_SPAN_V=$(hwm_field "$line" poison-span)
    HWM_HR_V=$(hwm_field "$line" headroom)
    HWM_DEEP_V=$(hwm_field "$line" deepest)
    HWM_LOW_V=$(printf '%s\n' "$line" | grep -oE 'lowest=0x[0-9a-fA-F]+' | head -1 | cut -d= -f2)
    if [ -z "${HWM_ALLOC_V:-}" ] || [ -z "${HWM_SPAN_V:-}" ] || \
       [ -z "${HWM_HR_V:-}" ] || [ -z "${HWM_DEEP_V:-}" ] || [ -z "${HWM_LOW_V:-}" ]; then
        CM="the $where hwm line is not the shape the gate writes (alloc, poison-span, headroom, deepest, lowest=0x...): '$line'"
        return 1
    fi
    if [ "$HWM_ALLOC_V" -ne "$HWM_ALLOC" ]; then
        CM="the $where scan covers $HWM_ALLOC_V bytes, not the gate's $HWM_ALLOC — the budget moved without this clause, or the report measured another region"
        return 1
    fi
    if [ "$HWM_SPAN_V" -le 0 ] || [ "$HWM_SPAN_V" -ge "$HWM_ALLOC_V" ]; then
        CM="the $where poisoned span is $HWM_SPAN_V of $HWM_ALLOC_V bytes — the create arm either armed nothing or poisoned the whole region, image included"
        return 1
    fi
    if [ "$HWM_DEEP_V" -le 0 ]; then
        CM="the $where scan found every poisoned byte untouched (deepest=0) — the workload never used the stack below the entry %rsp, or the scan reads a region the poison never reached"
        return 1
    fi
    if [ $((HWM_HR_V + HWM_DEEP_V)) -ne "$HWM_SPAN_V" ]; then
        CM="the $where numbers disagree with themselves: headroom $HWM_HR_V + deepest $HWM_DEEP_V is $((HWM_HR_V + HWM_DEEP_V)), not the poisoned span $HWM_SPAN_V"
        return 1
    fi
    if [ $((HWM_LOW_V % 4096)) -ne $((HWM_HR_V % 4096)) ]; then
        CM="the $where lowest address $HWM_LOW_V is not $HWM_HR_V bytes past a page-aligned base (offsets $((HWM_LOW_V % 4096)) vs $((HWM_HR_V % 4096))) — the line's two numbers describe different spans"
        return 1
    fi
    if [ "$HWM_HR_V" -lt "$HWM_MIN_HEADROOM" ]; then
        CM="the $where headroom is $HWM_HR_V bytes, under the $HWM_MIN_HEADROOM-byte floor — the workload has eaten into the second half of its 64 KiB budget"
        return 1
    fi
    return 0
}

p_b7() {   # the stack was measured at both boots, and its headroom holds
    local d="$1" b1="$d/boot1.log" b2="$d/boot2.log"
    local stop exitl adopted=0 stop_span stop_hr stop_deep
    if [ ! -f "$b1" ]; then
        CM="the recording has no boot1.log — the stop measurement cannot be checked, and the work half is where the stack's first number is taken"
        return 1
    fi
    stop=$(grep -aF "[E7-GATE] stack hwm (stop):" "$b1" 2>/dev/null | tail -1)
    if [ -z "$stop" ]; then
        CM="the work half carries no stack hwm line — the gate profile's create arm (the 0xA5 poison) or its stop report is gone, so the run no longer measures the stack it budgets"
        return 1
    fi
    hwm_parse "$stop" stop || return 1
    stop_span="$HWM_SPAN_V"; stop_hr="$HWM_HR_V"; stop_deep="$HWM_DEEP_V"

    grep -aqF "[ENV_PAYLOAD] linux task 'e7.gate.user.0' adopted at" "$b2" && adopted=1
    exitl=$(grep -aF "[E7-GATE] stack hwm (exit):" "$b2" 2>/dev/null | tail -1)
    exit_count=$(grep -acF "[E7-GATE] stack hwm (exit):" "$b2" 2>/dev/null)
    if [ "${exit_count:-0}" -gt 1 ]; then
        CM="the teardown reported the stack $exit_count times — process_kill and process_exit must not both fire for one task, or the scan is running twice on one exit"
        return 1
    fi

    if [ -z "$exitl" ]; then
        if [ "$adopted" -eq 1 ]; then
            CM="boot 1 measured the stack (deepest $stop_deep, headroom $stop_hr) but the resumed tenant's teardown has no exit scan — the report rides process_kill and process_exit, so an adopted tenant produces one on either path, fault included"
            return 1
        fi
        CM="boot 1 measured the stack (deepest $stop_deep, headroom $stop_hr of $HWM_ALLOC); this recording never adopted the tenant, so there is no resumed stack to scan at exit"
        return 0
    fi

    hwm_parse "$exitl" exit || return 1
    if [ "$adopted" -ne 1 ]; then
        CM="boot 1 measured the stack (deepest $stop_deep, headroom $stop_hr of $HWM_ALLOC); this recording never adopted the tenant, so its exit scan ($HWM_DEEP_V bytes) is not the resumed copy's and is not compared to the stop"
        return 0
    fi
    if [ "$HWM_SPAN_V" -ne "$stop_span" ]; then
        CM="the two boots disagree on the poisoned span (stop $stop_span, exit $HWM_SPAN_V) — the checkpoint did not carry the create's arming, so the second scan is not the same stack"
        return 1
    fi
    if [ "$HWM_DEEP_V" -lt "$stop_deep" ]; then
        CM="the resumed scan is shallower ($HWM_DEEP_V) than the stop ($stop_deep) — the checkpoint lost the touched pages' poison, so boot 2 is measuring a different stack"
        return 1
    fi
    CM="the stack is measured at both boots: deepest $HWM_DEEP_V bytes below the entry %rsp ($stop_deep at the stop, the resumed tail went $((HWM_DEEP_V - stop_deep)) deeper), $HWM_HR_V of $HWM_ALLOC bytes untouched — $((HWM_HR_V * 100 / HWM_ALLOC)).$((HWM_HR_V * 1000 / HWM_ALLOC % 10))% above the $HWM_MIN_HEADROOM-byte floor"
    return 0
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
        no-fd)      echo "B3 B4" ;;
        no-heap)    echo "B4 B5" ;;
        fs_base)    echo "B2 B4 B5" ;;

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
        fs_base)
            local line restored capture adopted
            line=$(grep -aF "[E7-GATE] tooth fs_base:" "$b2" | head -1)
            if [ -z "$line" ]; then
                CM="the re-stamped-TLS-base line ('[E7-GATE] tooth fs_base: ...') is missing — the recording cannot say which break produced the B2 red"
                return 1
            fi
            if printf '%s\n' "$line" | grep -q "NOT armed"; then
                CM="the tooth refused to arm ($line) — a B2 red from a walk that never happened would prove nothing"
                return 1
            fi
            restored=$(printf '%s\n' "$line" | sed -n 's/.*restored=0x\([0-9a-fA-F]*\).*/\1/p')
            capture=$(printf '%s\n' "$line" | sed -n 's/.*capture=0x\([0-9a-fA-F]*\).*/\1/p')
            adopted=$(grep -aF "[ENV_PAYLOAD] linux task 'e7.gate.user.0' adopted at" "$b2" | tail -1 | \
                          sed -n 's/.*fs_base=0x\([0-9a-fA-F]*\).*/\1/p')
            if [ -z "$restored" ] || [ -z "$capture" ] || [ -z "$adopted" ]; then
                CM="the tooth line or the adoption does not carry both bases (capture='${capture:-?}', restored='${restored:-?}', adopted='${adopted:-?}') — the red is not attributable"
                return 1
            fi
            if [ "$restored" = "$capture" ]; then
                CM="the tooth line names the same base twice (0x$capture) — the arm printed a re-stamp it did not make"
                return 1
            fi
            if [ "$adopted" != "$restored" ]; then
                CM="the adoption reports fs_base=0x$adopted, the tooth stamped 0x$restored — the value B2 reddens on is not the one installed"
                return 1
            fi
            CM="the record's fs_base=0x$capture was re-stamped 0x$restored and that is what the adoption installed — the TLS base is one page off the capture's, and the B2 red is the tooth's own"
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
    report_clause "$d" B7 b7 "$want"

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

if [ "$LIVE" -eq 0 ] && [ "$TEETH" -eq 0 ] && [ "$SELF" -eq 0 ]; then
    if [ "$fails" -ne 0 ]; then
        echo "e7_gate_check: $fails clause(s) failed"
        exit 1
    fi
    echo "e7_gate_check: the gate's source wiring holds (use --live for the run, --teeth for the breaks, --selfkill for the deferred teardown)"
    exit 0
fi

# ─── The live arms ─────────────────────────────────────────────────────────
if [ "$LIVE" -eq 1 ]; then
    DRILL="${E7G_DRILL:-1}"
    ISO="${E7G_ISO:-sls_operating_system_e3_drill.iso}"
elif [ "$SELF" -eq 1 ]; then
    DRILL=0
    ISO="${E7G_ISO:-sls_operating_system_e3_selfkill.iso}"
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
# The Makefile's knob is EMPTY-or-1: 0 is refused by name (it cannot be told
# apart from an unset value in the config stamp). The control build therefore
# passes the empty string; only the teeth build passes 1.
TOOTH_MAKE=""
[ "$TOOTH_BUILD" = "1" ] && TOOTH_MAKE=1
if [ "$BUILD" = "1" ]; then
    if [ "$SELF" -eq 1 ]; then
        echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SELFKILL=1 X86_E3_ISO=$ISO ==="
        make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SELFKILL=1 \
            X86_E3_ISO="$ISO" || fail "the self-kill image did not build"
    else
        echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT=$DRILL E7_TOOTH=${TOOTH_MAKE:-<empty>} X86_E3_ISO=$ISO ==="
        make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate E7_SLOW_BOOT="$DRILL" \
            E7_TOOTH="$TOOTH_MAKE" X86_E3_ISO="$ISO" || fail "the gate image did not build"
    fi
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
# so the two arms capture the SAME thing (one capture, five replays, is what
# makes the teeth a comparison rather than five unrelated runs).
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

    # A restore that poured nothing has no resumed workload to wait for: the
    # refusal (a create that could not reach its manager, or the P1a posture
    # window) is the CALLER's cue to retry on a fresh copy, and sitting out
    # E7G_WORKLOAD_S first would bury that retry under fifteen minutes of a
    # boot with no tenant in it. The report is the authority: payloads=0 means
    # no bytes were applied, whatever a background heal may try later.
    if [ "$(jval "$ard/restore.json" payloads)" = "0" ]; then
        # A recording even when nothing poured: facts are what the retry note
        # and a --replay read, and an attempt with no facts is indistinguishable
        # from a broken directory.
        {
            echo "payloads=0"
            echo "payload_skips=$(jval "$ard/restore.json" payload_skips)"
            echo "tooth=$(jval "$ard/restore.json" tooth)"
            echo "partition=$PID"
        } > "$ard/facts"
        echo "note: the restore poured nothing (payloads=0) — nothing to wait for; powering off"
        sleep 2
        power_off
        return 0
    fi

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

# ─── The self-kill arm: the deferred branch, observed live ─────────────────
# process_kill() defers a RUNNING target, and the scheduler tears it down on
# its next pass. Nothing else in the gate reaches that branch — every recorded
# kill targets a parked task — so this mode builds the one image whose gate
# stop asks for the running tenant's own death, then asserts the request, the
# deferral, the teardown and the scan in one boot. No checkpoint is involved:
# the tenant is killed at the copy continuation, so the workload cannot finish.
assert_selfkill() {
    local d="$1"
    local b="$d/boot1.log"
    local stop exitl n tl el stop_span stop_deep
    if [ ! -f "$b" ]; then
        bad "S1. the self-kill boot has no log at $b"
        return
    fi
    if grep -aqF "[E7-GATE] self-kill:" "$b" && \
       grep -aqE '\[PROC\] PID [0-9]+ kill deferred \(running\)' "$b"; then
        ok "S1. the kill was requested while the tenant was RUNNING and process_kill() deferred it ($(grep -aoE 'self-kill: process_kill\(pid=[0-9]+\)' "$b" | head -1))"
    else
        bad "S1. the self-kill request or its deferral line is missing — the stop arm did not fire, or process_kill() did not see the task RUNNING"
    fi
    if grep -aqE "\[PROC\] PID [0-9]+ '[^']*' deferred teardown \(killed while running\)" "$b"; then
        ok "S2. the scheduler's deferred branch tore the tenant down ($(grep -aoE "\[PROC\] PID [0-9]+ '[^']*' deferred teardown" "$b" | head -1))"
    else
        bad "S2. no deferred-teardown line — the scheduler never swept the task it had deferred"
    fi
    n=$(grep -acF "[E7-GATE] stack hwm (exit):" "$b")
    exitl=$(grep -aF "[E7-GATE] stack hwm (exit):" "$b" | tail -1)
    tl=$(grep -anE "deferred teardown \(killed while running\)" "$b" | head -1 | cut -d: -f1)
    if [ "$n" -eq 1 ] && [ -n "$exitl" ] && [ -n "$tl" ]; then
        el=$(grep -anF "[E7-GATE] stack hwm (exit):" "$b" | head -1 | cut -d: -f1)
        if [ "$el" -gt "$tl" ]; then
            ok "S3. exactly one stack scan, and it follows the deferred-teardown line (line $el after $tl) — the scan happens at the teardown, not at the request"
        else
            bad "S3. the exit scan (line $el) does not follow the deferred teardown (line $tl) — the scan is not being taken at the teardown"
        fi
    else
        bad "S3. expected exactly one exit scan after a deferred teardown; found $n exit line(s), teardown line '${tl:-none}'"
    fi
    stop=$(grep -aF "[E7-GATE] stack hwm (stop):" "$b" | tail -1)
    if [ -z "$stop" ]; then
        bad "S4. the stop measurement is missing — the arm killed the task before its stack was recorded"
        return
    fi
    hwm_parse "$stop" stop || { bad "S4. $CM"; return; }
    stop_span="$HWM_SPAN_V"; stop_deep="$HWM_DEEP_V"
    hwm_parse "$exitl" exit || { bad "S4. $CM"; return; }
    if [ "$HWM_SPAN_V" -ne "$stop_span" ]; then
        bad "S4. the deferred scan sees span $HWM_SPAN_V, the stop $stop_span — the same stack was not scanned"
    elif [ "$HWM_DEEP_V" -lt "$stop_deep" ]; then
        bad "S4. the deferred scan is shallower ($HWM_DEEP_V) than the stop ($stop_deep) — it is not the same stack's deepest point"
    else
        ok "S4. the deferred scan re-measures the same stack: deepest $HWM_DEEP_V below the entry %rsp ($stop_deep at the stop), $HWM_HR_V of $HWM_ALLOC untouched — $((HWM_HR_V * 100 / HWM_ALLOC)).$((HWM_HR_V * 1000 / HWM_ALLOC % 10))% above the floor"
    fi
    if [ "$(grep -acF '[backup] Done:' "$b")" -eq 0 ]; then
        ok "S5. the tenant was taken down mid-copy (no [backup] Done:) — the kill landed on the running workload, not after it finished"
    else
        bad "S5. the workload printed [backup] Done: before the deferred teardown — the kill did not land on the running copy"
    fi
}

if [ "$SELF" -eq 1 ]; then
    echo "=== self-kill: create, run to the stop, kill while RUNNING ==="
    rm -f "$IMG"
    truncate -s 10G "$IMG" || fail "could not make the NVMe image"
    boot "$L1" selfkill
    BASE="http://127.0.0.1:$PORT"

    api "$WD/pcreate.json" -X POST -d '{"name":"e7self"}' "$BASE/api/partitions" || fail "POST /api/partitions"
    PID=$(jval "$WD/pcreate.json" partition_id)
    [ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
    echo "partition=$PID"
    api "$WD/create.json" -X POST -d '{"index":1}' "$BASE/api/partition/$PID/env" || fail "POST env"
    echo "create: $(cat "$WD/create.json")"

    for i in $(seq 1 480); do
        grep -aqF "deferred teardown (killed while running)" "$L1" && break
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the deferred teardown"
        sleep 0.5
    done
    sleep 1
    power_off

    echo "=== self-kill clauses ==="
    assert_selfkill "$ART"
    if [ "$fails" -ne 0 ]; then
        echo "e7_gate_check: $fails clause(s) failed; artifacts in $ART"
        exit 1
    fi
    echo "e7_gate_check: the self-kill case is observed — deferred while RUNNING, torn down and scanned by the scheduler; artifacts in $ART"
    exit 0
fi

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
# One capture, then five replays over SPARSE COPIES of the post-checkpoint
# image: the control must be fully green (the arms do not fire unselected) and
# each tooth must redden exactly its declared set, with its own line in the
# transcript. The copy is `--sparse=always` so the 10 GiB image costs its
# allocated extents, not ten gigabytes multiplied by six.
rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"
gate_capture

echo "=== the teeth: one capture, five replays ==="
TC="$ART/teeth"
mkdir -p "$TC" || fail "cannot create $TC"
# The capture is the SOURCE for every copy: a replay must not be asked to
# carry the next tooth's checkpoint, and the record surviving a replay is a
# property of the record layer, not something this loop may lean on.
SRC_IMG="$IMG"
retries=0

# A replay whose pour was REFUSED never applied its tooth: all four breaks
# live in the pour or in the forward path after it. The refusals seen here are
# the P1a posture window (a sidecar still RUNNING when the wait's ceiling
# expires — env_payload.h's account of run 12) and a replay whose create could
# not reach the environment manager it boots with; neither is a tooth's red,
# so this arm retries that boot ON A FRESH COPY, prints the attempt and the
# recorded reason. The retry is bounded and visible; a replay that never gets
# its contents injected can never be counted as a pass for any tooth, and a
# pour that refuses EVERY attempt fails the run instead of being retried away.
replay_for_tooth() {   # <name> <tag-base> <body> -> 0 when the pour landed
    local name="$1" tag="$2" body="$3" try payloads skips
    for try in 1 2 3; do
        [ "$try" -gt 1 ] && retries=$((retries + 1))
        cp --sparse=always "$SRC_IMG" "$TC/$name.img" || \
            fail "could not copy the checkpointed image for $name"
        IMG="$TC/$name.img"
        rm -rf "$TC/$name"
        gate_replay "$TC/$name/boot2.log" "${tag}r$try" "$body" "$TC/$name"
        # The capture log belongs to every replay's recording: B7's stop
        # measurement is taken in boot 1, and a tooth directory carrying only
        # boot 2 could not be replayed on its own.
        cp "$ART/boot1.log" "$TC/$name/boot1.log" || \
            fail "could not attach the capture log to $name"
        payloads=$(fact "$TC/$name" payloads)
        skips=$(fact "$TC/$name" payload_skips)
        if [ "$payloads" = "1" ]; then return 0; fi
        reason=$(grep -aF "[ENV_RESTORE] last refusal:" "$TC/$name/boot2.log" 2>/dev/null | \
                     tail -1 | sed 's/.*last refusal: //')
        echo "note: $name attempt $try got no pour (payloads=${payloads:-?}, payload_skips=${skips:-?}) — ${reason:-a restore refusal, not this tooth}; retrying on a fresh copy"
    done
    return 1
}

i=0
for tooth in control descriptor no-fd no-heap fs_base; do
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
for tooth in control descriptor no-fd no-heap fs_base; do
    echo "--- $tooth"
    assert_artifacts "$TC/$tooth"
done

if [ "$fails" -ne 0 ]; then
    echo "e7_gate_check: $fails clause(s) failed; artifacts in $ART"
    exit 1
fi
if [ "$retries" -gt 0 ]; then
    echo "e7_gate_check: $retries replay(s) were retried for a restore refusal (a create that could not reach its manager, or the P1a posture window) — not a tooth's red; the recordings kept are the ones that poured"
fi
echo "e7_gate_check: the teeth redden by name — $red_count clause(s) red across descriptor/no-fd/no-heap/fs_base, each exactly where its tooth says, the control green; artifacts in $ART"
exit 0

#!/usr/bin/env bash
# m4_threads_wall_check.sh — the M4 source-only wall guard.
#
# M4 is the modern-runtime gate: Go, threaded Rust, threaded CPython, Node
# and every other threaded runtime need `clone`, `futex`, a shared address
# space and a thread group. The tree has M4.1's group object and refcount and
# M4.2's clone/thread-create path, but no futex. This guard has four jobs:
#
#   1. While all three mechanisms are absent, the design/roadmap surfaces
#      must keep Go and the threaded runtimes OUT.
#   2. A documented, ordered partial stage is allowed: M4.1 (group only) and
#      M4.2 (clone + group, no futex yet) are named in the design and must
#      still keep every runtime out. `futex` without clone/group, or clone
#      without a group, is out of order and fails.
#   3. All three mechanisms present is still not enough for a runtime claim:
#      the live M4.7 gate artifact must exist and carry its markers. F8
#      refuses an all-present tree with no artifact; a doc sentence is not
#      evidence.
#   4. Only then may the Linux-shape record mark the M4.7 gate landed; a
#      full-landing claim without the artifact reddens here.
#
# The source census is deliberately narrow: it looks for the Linux numbers
# and the group fields, not for the word `clone` in comments (the kernel has
# `user_clone_page_table()` and every process create uses it). A copied page
# table is the opposite of this milestone, so matching that symbol would be
# a false positive.
#
# Source-only: no build, no boot, no toolchain. It carries no GUARD-KIND
# marker because there is nothing it can legitimately be unable to do on a
# Linux runner.
#
# Run:  bash tests/m4_threads_wall_check.sh
#       bash tests/m4_threads_wall_check.sh <root>
#       M4_WALL_CHECK_ROOT=<root> bash tests/m4_threads_wall_check.sh
set -uo pipefail

ROOT="${M4_WALL_CHECK_ROOT:-${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}}"
cd "$ROOT" || exit 2

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails+1)); }

DOC=docs/AeroSLS-M4-Threads-Design-v0.1.md
PASE=docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md
LINUX=docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md
POSIX=docs/AeroSLS-POSIX-Environments-Roadmap-v0.2.md
ABI_C=kernel/linux_abi.c
ABI_H=kernel/linux_abi.h
PROC_H=kernel/process.h
PROC_C=kernel/process.c
CAP_C=kernel/cap.c
LINUX_THREADS_H=kernel/linux_threads.h
DISPATCH=kernel/syscall_dispatch.c
LINUX_RS=user/sidecar/src/linux.rs
CKPT_H=kernel/env_ckpt.h
CAND=tools/linux_abi_candidate.conf
CENSUS=tools/linux_syscall_census.txt

# ─── A. Preconditions ──────────────────────────────────────────────────────
for f in "$DOC" "$PASE" "$LINUX" "$POSIX" "$ABI_C" "$ABI_H" \
         "$PROC_H" "$PROC_C" "$CAP_C" "$LINUX_THREADS_H" "$DISPATCH" "$LINUX_RS" \
         "$CKPT_H" "$CAND" "$CENSUS"; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f missing — the M4 wall cannot be evaluated." >&2
        exit 2
    fi
done
ok "A. every file this guard reads exists"

# ─── B. The design exists and states the wall ─────────────────────────────
if grep -qE '^Status: M4\.[0-9]+ landed; M4\.[0-9]+ .*runtime wall in force\.$' "$DOC" ||
   grep -qE '^Status: M4\.7 gate landed; runtime wall lifted\.$' "$DOC"; then
    ok "B1. the design status names the landed increment and the correct wall posture"
else
    bad "B1. the M4 design has no truthful staged status line — an increment status is not a runtime landing"
fi

if grep -qF 'Go, threaded Rust, threaded CPython, Node, and every other threaded runtime remain out' "$DOC" ||
   grep -qF 'Go and every threaded runtime are supported' "$DOC"; then
    ok "B2. the design states the runtime posture (wall, or full gate with evidence)"
else
    bad "B2. the design no longer states the runtime posture — the wall is not optional while clone/futex/thread groups are absent"
fi

if grep -qF 'An undocumented or out-of-order partial implementation is a failure, not a milestone' "$DOC"; then
    ok "B3. the design refuses undocumented or out-of-order partial support"
else
    bad "B3. the design lost its partial-implementation rule — only the named M4.1/M4.2 stages may land early"
fi

# ─── C. The decisions are the contract ───────────────────────────────────
for d in 1 2 3 4 5 6 7 8; do
    if grep -qE "^\*\*D$d — " "$DOC"; then
        ok "C$d. decision D$d is present"
    else
        bad "C$d. decision D$d is missing from the M4 design"
    fi
done

# ─── D. The mechanisms the decisions name are the ones to build ──────────
for pair in \
    'D1|one shared address space|the design must say the group owns the shared address space' \
    'D2|user_destroy_page_table|the design must name the last-reference teardown' \
    'D3|syscall stack|the design must give each thread its own kernel stack' \
    'D4|SYS_SLS_LINUX_CLONE_THREAD|the design must name the clone mechanism' \
    'D5|CLONE_CHILD_CLEARTID|the design must preserve Linux child-tid semantics' \
    'D6|FUTEX_WAKE|the design must define futex wait/wake' \
    'D7|getpid|the design must distinguish TGID from TID' \
    'D8|checkpoint/restore|the design must make restore thread-aware'; do
    tag="${pair%%|*}"; needle="${pair#*|}"; why="${needle#*|}"; text="${needle%%|*}"
    if grep -qF "$text" "$DOC"; then
        ok "$tag. the design names '$text'"
    else
        bad "$tag. the design does not name '$text' — $why"
    fi
done

# ─── E. The increment plan cannot skip the gate ──────────────────────────
for n in 0 1 2 3 4 5 6 7 8; do
    if grep -qE "M4\.$n — " "$DOC"; then
        ok "E$n. M4.$n is planned"
    else
        bad "E$n. M4.$n is missing — the milestone cannot skip from design to Go"
    fi
done

if grep -qF 'static Go candidate' "$DOC" && grep -qF 'checkpoint/restore cycle' "$DOC"; then
    ok "E9. the final gate is a static Go candidate across a checkpoint/restore cycle"
else
    bad "E9. the final gate no longer names a static Go candidate and a checkpoint/restore cycle"
fi

# ─── F. The source census: absent, staged partial, or all present ─────────
clone_hits="$(grep -nE 'LINUX_CLONE|SYS_LINUX_CLONE|^[[:space:]]*\{[[:space:]]*56,[[:space:]]*LSC_' \
    "$ABI_C" "$ABI_H" "$DISPATCH" "$LINUX_RS" 2>/dev/null || true)"
futex_hits="$(grep -nE 'LINUX_FUTEX|SYS_LINUX_FUTEX|^[[:space:]]*\{[[:space:]]*202,[[:space:]]*LSC_' \
    "$ABI_C" "$ABI_H" "$DISPATCH" "$LINUX_RS" 2>/dev/null || true)"
group_hits="$(grep -nE 'LinuxThreadGroup|linux_tgid|thread_group_id|linux_thread_group' \
    "$PROC_H" "$PROC_C" "$CAP_C" "$LINUX_THREADS_H" 2>/dev/null || true)"

clone=0; futex=0; group=0
[ -n "$clone_hits" ] && clone=1
[ -n "$futex_hits" ] && futex=1
[ -n "$group_hits" ] && group=1
support=$((clone + futex + group))

# The live M4.7 artifact is the ONLY thing that can unlock a runtime claim.
# It must be a real file with every positive marker and no failure marker.
M4_EVIDENCE="${M4_EVIDENCE:-evidence/m4_runtime_gate.log}"
gate_ok=0
if [ -s "$M4_EVIDENCE" ] &&
   grep -q 'M4-GO-OK' "$M4_EVIDENCE" &&
   grep -q 'M4-FUTEX-OK' "$M4_EVIDENCE" &&
   grep -q 'M4-CHECKPOINT-OK' "$M4_EVIDENCE" &&
   grep -q 'M4-RUNTIME-GATE-PASS' "$M4_EVIDENCE" &&
   ! grep -q 'M4-RUNTIME-GATE-FAIL' "$M4_EVIDENCE"; then
    gate_ok=1
fi

if [ "$support" -eq 0 ]; then
    ok "F1. clone, futex and the thread-group field are absent from the source census (M4.0 wall)"
    stage=0
elif [ "$group" -eq 1 ] && [ "$clone" -eq 0 ] && [ "$futex" -eq 0 ]; then
    ok "F2. M4.1 is the landed stage: group/refcount present, clone and futex absent"
    stage=1
elif [ "$group" -eq 1 ] && [ "$clone" -eq 1 ] && [ "$futex" -eq 0 ]; then
    ok "F3. M4.2 is the landed stage: clone and group present, futex absent"
    stage=2
elif [ "$group" -eq 1 ] && [ "$clone" -eq 1 ] && [ "$futex" -eq 1 ]; then
    ok "F4. clone, futex and thread groups are all present in the source census"
    stage=3
else
    bad "F5. M4 is out of order (clone=$clone futex=$futex group=$group) — futex requires clone+group and clone requires a group; only the documented M4.1/M4.2 stages may be partial"
    stage=-1
fi

# Only all three mechanisms plus the live artifact can lift the runtime wall.
# Everything else, including an all-present tree with no artifact, stays out.
runtime_unlocked=0
if [ "$stage" -eq 3 ] && [ "$gate_ok" -eq 1 ]; then
    runtime_unlocked=1
fi

# The current designated candidate must still be the single-threaded backup
# workload. A threaded candidate designation without M4 evidence would turn
# the census into a claim the gate cannot support.
if grep -q '^CANDIDATE_STATE=designated$' "$CAND" &&
   grep -q '^CANDIDATE_PROVENANCE=backup/backup\.sh$' "$CAND"; then
    ok "F6. the designated candidate is still the single-threaded backup workload"
else
    bad "F6. the designated candidate changed without M4 evidence — the wall guard must be updated with the gate that justifies it"
fi

if ! grep -qE '^(56|202)[[:space:]]' "$CENSUS"; then
    ok "F7. the measured census still has no clone/futex row (the candidate is single-threaded)"
else
    bad "F7. the census now contains clone/futex — the single-threaded candidate claim is stale"
fi

if [ "$stage" -eq 3 ]; then
    if [ "$gate_ok" -eq 1 ]; then
        ok "F8. the live M4.7 gate artifact carries GO, FUTEX, CHECKPOINT and PASS with no FAIL"
    else
        bad "F8. all three mechanisms are present but the live M4.7 gate artifact is missing — an all-present tree may not land without real gate evidence"
    fi
elif [ "$gate_ok" -eq 1 ]; then
    ok "F8. a live M4.7 gate artifact is present, but the documented partial stage is still the wall"
else
    ok "F8. no live M4.7 gate artifact is present; the documented partial stage keeps the runtime wall"
fi

# ─── G. The docs must agree with the source state ─────────────────────────
case "$stage" in
    0)
        if grep -qE '^Status: M4\.0|Design spike landed 2026-11-14' "$DOC" "$PASE"; then
            ok "G1. the M4.0 stage is documented"
        else
            bad "G1. no M4.0 stage is documented while all three mechanisms are absent"
        fi
        ;;
    1)
        if grep -q 'M4\.1 landed 2026-11-14' "$PASE" &&
           grep -q 'M4\.2 not started' "$PASE" &&
           grep -q 'M4\.7 gate not evidenced' "$PASE" "$DOC"; then
            ok "G1. M4.1 is documented as landed with M4.2 not started and the M4.7 gate not evidenced"
        else
            bad "G1. M4.1's group/refcount is present but the docs do not state M4.1 landed / M4.2 not started / M4.7 gate not evidenced"
        fi
        ;;
    2)
        if grep -q 'M4\.2 landed' "$PASE" &&
           grep -q 'M4\.7 gate not evidenced' "$PASE" "$DOC"; then
            ok "G1. M4.2 is documented as landed with the M4.7 gate not evidenced"
        else
            bad "G1. clone+group are present but the docs do not state M4.2 landed and the M4.7 gate not evidenced"
        fi
        ;;
    3)
        if [ "$gate_ok" -eq 1 ]; then
            if grep -q 'M4\.7 gate LANDED' "$PASE"; then
                ok "G1. all mechanisms and the live gate artifact are present, and the record marks the M4.7 gate landed"
            else
                bad "G1. the live gate artifact exists but the Linux-shape record does not mark 'M4.7 gate LANDED'"
            fi
        elif grep -q 'M4\.7 gate not evidenced' "$PASE" "$DOC" &&
             ! grep -qE 'M4\.7 gate LANDED|Go is supported|Go and every threaded runtime are supported|threaded runtimes are supported' "$DOC" "$PASE" "$LINUX" "$POSIX" 2>/dev/null; then
            ok "G1. all mechanisms are present without evidence; F8 refuses the landing and the docs keep the wall"
        else
            bad "G1. all mechanisms are present and the docs claim the M4.7 gate/runtime without a live gate artifact — a doc line is not evidence"
        fi
        ;;
    *)
        bad "G1. no coherent M4 stage can be checked from the source census"
        ;;
esac

if [ "$runtime_unlocked" -eq 1 ]; then
    if grep -q 'AeroSLS-M4-Threads-Design-v0\.1\.md' "$LINUX" &&
       grep -q 'M4\.7 gate LANDED' "$LINUX" "$POSIX" 2>/dev/null; then
        ok "G2. with the live gate artifact, the Linux design points at M4 and records the M4.7 landing"
    else
        bad "G2. the live gate artifact exists but the Linux design/POSIX roadmap do not record the M4.7 landing"
    fi
else
    if grep -q 'AeroSLS-M4-Threads-Design-v0\.1\.md' "$LINUX" &&
       grep -qF 'not implemented' "$LINUX"; then
        ok "G2. the Linux ABI design points at the M4 design and says the remaining mechanisms are not implemented"
    else
        bad "G2. the Linux ABI design does not point at the M4 design or no longer says the unbuilt mechanisms are not implemented"
    fi
fi

if [ "$runtime_unlocked" -eq 1 ]; then
    if grep -q 'AeroSLS-M4-Threads-Design-v0\.1\.md' "$POSIX" &&
       grep -q 'M4\.7 gate LANDED' "$POSIX"; then
        ok "G3. the POSIX roadmap records the M4.7 gate landing"
    else
        bad "G3. the POSIX roadmap does not record the M4.7 gate landing after the live artifact"
    fi
else
    if grep -q 'AeroSLS-M4-Threads-Design-v0\.1\.md' "$POSIX" &&
       grep -q 'not landed' "$POSIX"; then
        ok "G3. the POSIX roadmap points at the M4 design and keeps the full gap not landed"
    else
        bad "G3. the POSIX roadmap does not point at the M4 design or no longer says the full gap is not landed"
    fi
fi

if [ "$stage" -lt 3 ]; then
    if grep -qE 'M4\.7 gate LANDED|Go is supported|Go and every threaded runtime are supported|threaded runtimes are supported' \
        "$DOC" "$PASE" "$LINUX" "$POSIX" 2>/dev/null; then
        bad "G4. a roadmap claims the M4.7 gate or runtime support while clone/futex/thread groups are not all present"
    else
        ok "G4. no roadmap claims the M4.7 gate or runtime support before all mechanisms are present"
    fi
else
    if [ "$gate_ok" -eq 1 ]; then
        ok "G4. the runtime claim is backed by the live gate artifact"
    elif grep -qE 'M4\.7 gate LANDED|Go is supported|Go and every threaded runtime are supported|threaded runtimes are supported' \
        "$DOC" "$PASE" "$LINUX" "$POSIX" 2>/dev/null; then
        bad "G4. the docs claim the M4.7 gate or runtime support without a live artifact — F8 refuses the all-present landing"
    else
        ok "G4. no runtime claim is made without the live gate artifact; F8 owns the all-present refusal"
    fi
fi

# ─── H. The wall's own consequences stay visible ─────────────────────────
if grep -q 'Go' "$DOC" && grep -qi 'thread' "$DOC" && grep -q 'futex' "$DOC"; then
    ok "H1. the design names Go, threads and futex together"
else
    bad "H1. the design no longer names Go/threads/futex together — the runtime consequence is not recorded"
fi

if grep -q 'ENV_CKPT_MAX_TASKS' "$CKPT_H" && grep -q 'ENV_CKPT_MAX_TASKS' "$DOC"; then
    ok "H2. the checkpoint task bound is named on both the source and the design side"
else
    bad "H2. the checkpoint task bound is not named on both sides — a thread group cannot be restored through the one-task shape"
fi

echo
if [ "$fails" -gt 0 ]; then
    echo "m4_threads_wall_check: FAIL ($fails clause(s))"
    exit 1
fi
echo "m4_threads_wall_check: OK — M4 is scoped and the runtime wall is explicit"
exit 0

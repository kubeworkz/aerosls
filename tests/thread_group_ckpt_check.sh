#!/usr/bin/env bash
# tests/thread_group_ckpt_check.sh — E7 TG-2d's two-thread checkpoint arm.
#
#   (no arguments)  source clauses only (no build, no QEMU)
#   --live          the source clauses, then the dedicated E7 gate build
#                   (`E7_GROUP_CKPT=1`) and the two-boot capture/replay arm
#
# The live half is the `--group` mode of tests/e7_gate_check.sh, so the arm
# uses the same partition/create/checkpoint/restore/QEMU harness as the
# single-task gate rather than a second, drifting copy. What is different is
# the workload: a freestanding two-thread fixture forms a one-word futex
# barrier, the kernel prints the whole-group marker only after both threads
# park, and the replay must adopt both contexts, leave both BLOCKED until the
# restored worker timeout wakes main, and finish the same checksum.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

LIVE=0
case "${1:-}" in
    "")      ;;
    --live)  LIVE=1 ;;
    -*)      echo "ABORT: unknown argument '$1' (usage: $0 [--live])" >&2; exit 2 ;;
    *)       echo "ABORT: unknown argument '$1' (usage: $0 [--live])" >&2; exit 2 ;;
esac

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
has() { grep -qF -- "$2" "$ROOT/$1" 2>/dev/null; }

missing=""
for f in tools/e7_group_ckpt.c user/sidecar/fixtures/e7_group_ckpt.elf \
         user/sidecar/src/e7_group_ckpt.rs kernel/linux_thread.c \
         kernel/process.c kernel/cap.c kernel/env_ckpt.c \
         kernel/env_payload.c tests/e7_gate_check.sh; do
    [ -s "$ROOT/$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
    bad "T1. the TG-2d files are missing:$missing"
else
    ok "T1. the fixture, sidecar profile, sibling constructor and live harness are present"
fi

if has user/sidecar/Cargo.toml "e7_group_ckpt = []" && \
   has Makefile "SIDECAR_GROUP_CKPT" && \
   has Makefile "E7_GROUP_CKPT ?=" && \
   has user/sidecar/src/entry.rs 'crate::e7_group_ckpt::stage(&mut booted.proc.vfs, 0)' && \
   has user/sidecar/src/entry.rs 'crate::e7_group_ckpt::spawn('; then
    ok "T2. the group workload is a build-time sidecar profile, staged and spawned from the gate tenant (never the shipped image)"
else
    bad "T2. the sidecar build feature or its stage/spawn call is missing — --live would boot a plain single-task gate"
fi

if has kernel/cap.c "env_ckpt_linux_group_restore_indexes" && \
   has kernel/cap.c "linux_restore_group_siblings" && \
   has kernel/process.c "[SIDECAR] restore group %u: created %u held sibling(s) on one root" && \
   has kernel/process.c "linux_group_join_restored_thread" && \
   has kernel/process.c "stacks[i] = 0;   /* the descriptor owns it from here */"; then
    ok "T3. the replay create reads the recorded sibling indexes, builds the whole set under the leader's group, and attaches each at its exact index"
else
    bad "T3. the replay create does not build the captured sibling set (or has lost the all-or-nothing stack preflight)"
fi

if has kernel/linux_thread.c "linux_group_join_restored_thread" && \
   has kernel/linux_thread.c "linux_group_futex_barrier_complete" && \
   has kernel/process.c "[E7-GATE] group barrier: group=%u threads=%u" && \
   has kernel/process.c "live >= 2" && \
   has kernel/env_payload.c "linux_futex_wait_enqueue(fgid, tk.futex_uaddr, pd->tid,"; then
    ok "T4. the kernel prints its checkpoint marker only for a whole group at one futex, and the pour re-enqueues every wait before release"
else
    bad "T4. the whole-group barrier predicate, marker, or payload re-enqueue is missing"
fi

if has tests/linux_thread_group_host_test.c "a restored sibling joins at the RECORDED index" && \
   has tests/linux_thread_group_host_test.c "the barrier predicate reports the whole group only when both waits share the word" && \
   has tests/env_ckpt_host_test.c "the replay create reads the sibling index the record captured" && \
   has tests/linux_thread_group_host_test.c "one release clears both holds and leaves both futex waiters BLOCKED"; then
    ok "T5. focused host tests cover exact-index attachment, the whole-group predicate, the record index reader, and the one-pass release"
else
    bad "T5. a focused host clause for TG-2d is missing"
fi

if has tools/e7_group_ckpt.c "G2-WORKER-TIMEOUT" && \
   has tools/e7_group_ckpt.c "G2-SHARED=1" && \
   has tools/e7_group_ckpt.c "G2-SUM=" && \
   has tests/e7_gate_check.sh "0xa97a7a6074da2325" && \
   has tests/e7_gate_check.sh "assert_group()"; then
    ok "T6. the live arm asserts the post-restore wake pair, the shared pre-barrier write, the resumed checksum and exactly one teardown"
else
    bad "T6. the live arm's facts are not wired (fixture markers, expected checksum or assert_group)"
fi

if [ "$fails" -ne 0 ]; then
    echo "thread_group_ckpt_check: $fails clause(s) failed"
    exit 1
fi
echo "thread_group_ckpt_check: the TG-2d source wiring holds"

if [ "$LIVE" -eq 1 ]; then
    exec bash "$ROOT/tests/e7_gate_check.sh" "$ROOT" --group
fi
echo "thread_group_ckpt_check: use --live for the two-boot capture/replay arm"

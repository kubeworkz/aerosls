#!/usr/bin/env bash
# tests/stack_frame_budget_smoke.sh — proves the stack-frame-budget
# guard's teeth bite, by planting a frame that exceeds the hard-fail line
# and asserting the guard fails on it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/stack_frame_budget_check.sh asserts the RELATIONAL property that
# -Wframe-larger-than cannot: every function's stack frame must fit the
# stack that the linked binary actually has (hard fail at FAIL_PCT=50%,
# advisory at WARN_PCT=25%). Its teeth were proven once, by hand, on a
# local build. Nothing ever made it fail automatically, so a regression
# that made the guard blind — a broken awk parse, a dropped tier, a
# mistyped FAIL_PCT — would pass every push unnoticed. This smoke proves
# the hard-fail tier bites: it plants a source file whose frame is 60% of
# the binary's real stack (over the 50% line, under the 100% overflow
# line, so exactly the FAIL tier fires) and asserts the guard exits 1
# naming it.
#
# ─── Why the tooth is sized from the binary, not hardcoded ─────────────────
# The guard reads the stack size from the binary's own stack_bottom/
# stack_top symbols — never a constant — because a threshold that drifts
# out of step with the stack is a check that has stopped meaning anything.
# The tooth obeys the same rule: the planted buffer is STACK * 60 / 100,
# so the tooth bites at any stack size the build happens to have.
#
# ─── How the tooth is planted and restored ─────────────────────────────────
# The guard compiles every kernel/*.c itself with -Wframe-larger-than, so
# the violation is a new file kernel/zz_frame_tooth.c (self-contained, no
# includes; volatile keeps -O2 from eliding the buffer). The guard also
# ABORTs on a STALE binary — any source newer than the image — so the
# tooth file's mtime is set to the binary's (`touch -r`), which is exactly
# the state a real build leaves behind. The file is removed afterwards;
# the trap removes it on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the tree on purpose and needs a build — it must
# never run in the local guard gate or a deploy gate. Run it (and the
# other build-host smokes) via tests/run_guard_smokes.sh after `make`.
#
# Exit: 0 if the tooth bit, 1 if it did not, 2 if the build is missing.
set -u

cd "$(dirname "$0")/.."   # repo root
guard=tests/stack_frame_budget_check.sh
fails=0
KERNEL=my_sls_kernel.bin
TOOTH=kernel/zz_frame_tooth.c

[ -f "$KERNEL" ] || { echo "ABORT: $KERNEL not found — run 'make' first (build-host smoke)" >&2; exit 2; }
command -v nm >/dev/null || { echo "ABORT: nm not found (binutils)" >&2; exit 2; }

trap 'rm -f "$TOOTH"' EXIT

SB="$(nm "$KERNEL" | awk '$3=="stack_bottom"{print $1}')"
ST="$(nm "$KERNEL" | awk '$3=="stack_top"{print $1}')"
if [ -z "$SB" ] || [ -z "$ST" ]; then
    echo "ABORT: stack_bottom/stack_top not in $KERNEL (broken build?)" >&2
    exit 2
fi
STACK=$(( 0x$ST - 0x$SB ))
BUFSZ=$(( STACK * 60 / 100 ))   # > 50% hard-fail line, < 100% overflow line
[ "$BUFSZ" -gt 0 ] || { echo "ABORT: computed stack size $STACK is not positive" >&2; exit 2; }

cat > "$TOOTH" <<EOF
__attribute__((noinline)) void zz_frame_tooth(void) {
    volatile char buf[$BUFSZ];
    buf[0] = 1;
    buf[sizeof(buf) - 1] = 2;
    (void)buf;
}
EOF
touch -r "$KERNEL" "$TOOTH"   # byte-for-byte the state a real build leaves

out="$(bash "$guard" 2>&1)"
rc=$?
rm -f "$TOOTH"

expected="FAILED: the largest frame"
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
    echo "TOOTH OK   over-budget frame — guard failed with: $expected"
    printf '%s\n' "$out" | grep -E "FAILED|largest frame" | sed 's/^/           /'
else
    echo "TOOTH FAIL over-budget frame — guard rc=$rc, expected failure '$expected'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Final restore check — prove the tree is byte-identical again, so this
# smoke can never leave a footprint for a later step (or a local run).
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — tree clean after tooth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the tooth was removed:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "stack_frame_budget_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]

#!/usr/bin/env bash
# m4_threads_wall_check_smoke.sh — teeth for tests/m4_threads_wall_check.sh.
#
# A guard nobody has watched fail is a guard nobody trusts. This smoke
# mutates COPIES of the exact inputs the wall guard reads, and requires the
# guard to exit 1 naming the clause that owns the property. The real tree is
# never touched.
#
# The teeth cover the ways the wall could rot:
#   - the documentation stops saying Go/threaded runtimes are out while the
#     mechanisms are absent or only partially built;
#   - the tree grows an out-of-order partial surface, or a documented stage's
#     status is missing/stale;
#   - the docs claim the M4.7 gate or runtime support without the live
#     evidence artifact, or the artifact is absent while the docs claim it.
#
# A missing design is an abort (exit 2), not a silent pass: the guard cannot
# evaluate a wall it cannot read.
#
# Run:  bash tests/m4_threads_wall_check_smoke.sh
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

GUARD=tests/m4_threads_wall_check.sh
FILES=(
    docs/AeroSLS-M4-Threads-Design-v0.1.md
    docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md
    docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md
    docs/AeroSLS-POSIX-Environments-Roadmap-v0.2.md
    kernel/linux_abi.c
    kernel/linux_abi.h
    kernel/process.h
    kernel/process.c
    kernel/cap.c
    kernel/linux_threads.h
    kernel/syscall_dispatch.c
    user/sidecar/src/linux.rs
    kernel/env_ckpt.h
    tools/linux_abi_candidate.conf
    tools/linux_syscall_census.txt
)

for f in "${FILES[@]}" "$GUARD"; do
    [ -f "$f" ] || { echo "ABORT: $f missing." >&2; exit 2; }
done

W="$(mktemp -d)" || { echo "ABORT: mktemp failed." >&2; exit 2; }
cleanup() { rm -rf "$W"; return 0; }
trap cleanup EXIT

seed() {
    local d="$1" f
    rm -rf "$d"
    for f in "${FILES[@]}"; do
        mkdir -p "$d/$(dirname "$f")"
        cp "$f" "$d/$f" || return 1
    done
    return 0
}

run_guard() { M4_WALL_CHECK_ROOT="$1" bash "$GUARD" 2>&1; }

pass=0
fail=0

tooth() {
    local name="$1" clause="$2" desc="$3" d="$4" out rc named=0
    out="$(run_guard "$d")"; rc=$?
    case "$out" in
        *"FAIL: $clause."*) named=1 ;;
    esac
    if [ "$rc" -eq 1 ] && [ "$named" -eq 1 ]; then
        echo "TOOTH OK   $name — guard exited 1 naming $clause"
        pass=$((pass+1))
    else
        echo "TOOTH FAIL $name — rc=$rc (want 1), clause '$clause' not named"
        printf '%s\n' "$out" | grep 'FAIL:' | head -4 | sed 's/^/           /'
        fail=$((fail+1))
    fi
    echo "           $desc"
}

# expect_pass <name> <description> <dir> — a state the wall guard must ALLOW.
# Used for the documented partial stages that are not a red (M4.1, M4.2, M4.3)
# and
# the fully evidenced gate once the record is updated to match.
expect_pass() {
    local name="$1" desc="$2" d="$3" out rc
    out="$(run_guard "$d")"; rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "STAGE OK   $name — guard allows the documented stage"
        pass=$((pass+1))
    else
        echo "STAGE FAIL $name — rc=$rc (want 0)"
        printf '%s\n' "$out" | grep 'FAIL:' | head -4 | sed 's/^/           /'
        fail=$((fail+1))
    fi
    echo "           $desc"
}

# ─── Baseline on a copy ────────────────────────────────────────────────────
D="$W/base"
seed "$D" || { echo "ABORT: could not seed the baseline." >&2; exit 2; }
if out="$(run_guard "$D")"; then
    echo "BASE OK    unmutated copy passes"
    pass=$((pass+1))
else
    echo "BASE FAIL  unmutated copy does not pass:"
    printf '%s\n' "$out" | sed 's/^/           /' | head -20
    fail=$((fail+1))
fi

# ─── Documentation teeth ──────────────────────────────────────────────────
D="$W/t1"; seed "$D" || exit 2
sed -i 's/^Status: M4\.4 landed; M4\.7 gate not evidenced; runtime wall in force\.$/Status: M4.4 landed; M4.7 gate landed; runtime supported./' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth status-landed B1 "the design starts claiming implementation" "$D"

D="$W/t2"; seed "$D" || exit 2
sed -i 's/Go, threaded Rust, threaded CPython, Node, and every other threaded runtime remain out/Threaded runtimes are supported/' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth go-supported B2 "the design drops the runtime wall" "$D"

D="$W/t2b"; seed "$D" || exit 2
sed -i 's/Go, threaded Rust, threaded CPython, Node, and every other threaded runtime remain out/Go and every threaded runtime are supported/' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth support-without-evidence G4 \
    "a support claim survives B2 but is still refused while the mechanisms are partial" "$D"

D="$W/t3"; seed "$D" || exit 2
sed -i 's/An undocumented or out-of-order partial implementation is a failure, not a milestone:/A partial implementation is acceptable:/' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth partial-rule B3 "partial support becomes acceptable" "$D"

D="$W/t4"; seed "$D" || exit 2
sed -i '/^\*\*D5 — /d' "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth missing-d5 C5 "a decision disappears from the contract" "$D"

D="$W/t5"; seed "$D" || exit 2
sed -i '/M4\.5 — Identity/d' "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth missing-m45 E5 "the exit/identity increment disappears" "$D"

D="$W/t6"; seed "$D" || exit 2
sed -i 's/M4\.1 landed 2026-11-14//g; s/M4\.2 landed 2026-11-14//g; s/M4\.2 landed//g; s/M4\.3 landed 2026-11-14//g; s/M4\.3 landed//g; s/M4\.4 landed 2026-11-14//g; s/M4\.4 landed//g' \
    "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
tooth pase-status-gone G1 "the Linux-shape record loses the spike status" "$D"

D="$W/t7"; seed "$D" || exit 2
sed -i 's/AeroSLS-M4-Threads-Design-v0\.1\.md/M4 Threads Design/' \
    "$D/docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md"
tooth linux-design-pointer-gone G2 "the Linux ABI design loses its M4 pointer" "$D"

D="$W/t8"; seed "$D" || exit 2
sed -i 's/AeroSLS-M4-Threads-Design-v0\.1\.md/M4 Threads Design/' \
    "$D/docs/AeroSLS-POSIX-Environments-Roadmap-v0.2.md"
tooth posix-pointer-gone G3 "the POSIX roadmap loses its M4 pointer" "$D"

D="$W/t9"; seed "$D" || exit 2
sed -i 's/`ENV_CKPT_MAX_TASKS`/the checkpoint task bound/g' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth checkpoint-bound-gone H2 "the design stops naming the current checkpoint bound" "$D"

D="$W/t10"; seed "$D" || exit 2
printf '\nM4.7 gate LANDED\n' >> \
    "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
tooth false-landed G4 "a roadmap claims the M4.7 gate landed while the mechanisms are absent" "$D"

# ─── Source-census teeth ──────────────────────────────────────────────────
D="$W/t11"; seed "$D" || exit 2
sed -i 's/M4\.3 landed/M4.3 not started/g; s/M4\.4 landed/M4.4 not started/g' \
    "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
sed -i 's/^Status: M4\.4 landed; M4\.7 gate not evidenced; runtime wall in force\.$/Status: M4.2 landed; M4.3 not started; M4.4 not started; M4.7 gate not evidenced; runtime wall in force./' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
tooth clone-without-m43-docs G1 \
    "clone+group+split is the M4.3 stage, but the docs still say M4.3 not started" "$D"

D="$W/t11b"; seed "$D" || exit 2
expect_pass m44-stage \
    "the landed clone+futex stage with M4.4 documented and the runtime wall in force" "$D"

D="$W/t12"; seed "$D" || exit 2
# Start from the group-only shape by removing every source-census clone hit,
# then add futex: that is the out-of-order combination F5 must refuse.
sed -i '/LINUX_CLONE/d' "$D/kernel/linux_abi.h" "$D/kernel/syscall_dispatch.c" \
    "$D/user/sidecar/src/linux.rs"
printf '\n#define LINUX_FUTEX 202\n' >> "$D/kernel/linux_abi.h"
tooth futex-only F5 "futex appears without clone and a group" "$D"

D="$W/t13"; seed "$D" || exit 2
printf '\n#define LINUX_CLONE 56\n#define LINUX_FUTEX 202\n' >> "$D/kernel/linux_abi.h"
sed -i 's/struct FutexWaiter/struct FutexSlot/g; s/fn answer_futex/fn answer_wait/g; s/PENDING_FUTEX/PENDING_WAIT/g; s/FUTEX_WAKE/FUTEX_UP/g' \
    "$D/user/sidecar/src/linux.rs"
sed -i 's/M4\.4 landed/M4.4 not started/g' \
    "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
tooth all-present-no-evidence F8 \
    "all three mechanisms appear without the documented M4.4 stage or live artifact" "$D"

D="$W/t13b"; seed "$D" || exit 2
printf '\n#define LINUX_CLONE 56\n#define LINUX_FUTEX 202\n' >> "$D/kernel/linux_abi.h"
mkdir -p "$D/evidence"
printf 'M4-GO-OK\nM4-FUTEX-OK\nM4-CHECKPOINT-OK\nM4-RUNTIME-GATE-PASS\n' \
    > "$D/evidence/m4_runtime_gate.log"
tooth evidence-without-landing G1 \
    "the live artifact exists but no roadmap records the M4.7 landing" "$D"

D="$W/t14"; seed "$D" || exit 2
printf '\n#define LINUX_CLONE 56\n#define LINUX_FUTEX 202\n' >> "$D/kernel/linux_abi.h"
mkdir -p "$D/evidence"
printf 'M4-GO-OK\nM4-FUTEX-OK\nM4-CHECKPOINT-OK\nM4-RUNTIME-GATE-PASS\n' \
    > "$D/evidence/m4_runtime_gate.log"
printf '\nM4.7 gate LANDED\n' >> "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
printf '\nM4.7 gate LANDED\n' >> "$D/docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md"
printf '\nM4.7 gate LANDED\n' >> "$D/docs/AeroSLS-POSIX-Environments-Roadmap-v0.2.md"
sed -i 's/^Status: M4\.1 landed; M4\.2 not started; runtime wall in force\.$/Status: M4.7 gate landed; runtime wall lifted./' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
sed -i 's/Go, threaded Rust, threaded CPython, Node, and every other threaded runtime remain out/Go and every threaded runtime are supported/' \
    "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
expect_pass full-gate-evidence \
    "the live artifact plus the documented landing lifts the wall" "$D"

D="$W/t14b"; seed "$D" || exit 2
printf '\n#define LINUX_CLONE 56\n#define LINUX_FUTEX 202\n' >> "$D/kernel/linux_abi.h"
printf '\nM4.7 gate LANDED\n' >> "$D/docs/AeroSLS-PASE-Linux-Shape-Decision-v0.1.md"
tooth all-present-false-landing G1 \
    "all three mechanisms appear with a doc-only gate claim and no artifact" "$D"

D="$W/t15"; seed "$D" || exit 2
sed -i 's/^CANDIDATE_PROVENANCE=backup\/backup\.sh$/CANDIDATE_PROVENANCE=some\/threaded\/candidate/' \
    "$D/tools/linux_abi_candidate.conf"
tooth candidate-changed F6 "the designated candidate changes without M4 evidence" "$D"

D="$W/t16"; seed "$D" || exit 2
printf '56 clone 1\n' >> "$D/tools/linux_syscall_census.txt"
tooth census-threaded F7 "the measured census grows a clone row" "$D"

# ─── Missing design is an abort, not a pass ───────────────────────────────
D="$W/t17"; seed "$D" || exit 2
rm -f "$D/docs/AeroSLS-M4-Threads-Design-v0.1.md"
out="$(run_guard "$D")"; rc=$?
case "$out" in
    *"ABORT:"*"AeroSLS-M4-Threads-Design-v0.1.md"*) named=1 ;;
    *) named=0 ;;
esac
if [ "$rc" -eq 2 ] && [ "$named" -eq 1 ]; then
    echo "TOOTH OK   no-design — guard aborted naming the missing design"
    pass=$((pass+1))
else
    echo "TOOTH FAIL no-design — rc=$rc (want 2), missing design not named"
    printf '%s\n' "$out" | sed 's/^/           /' | head -4
    fail=$((fail+1))
fi

# ─── Final baseline on the real tree ──────────────────────────────────────
if out="$(M4_WALL_CHECK_ROOT="$(pwd)" bash "$GUARD" 2>&1)"; then
    echo "REAL OK    the actual tree passes the guard"
    pass=$((pass+1))
else
    echo "REAL FAIL  the actual tree does not pass:"
    printf '%s\n' "$out" | sed 's/^/           /' | head -20
    fail=$((fail+1))
fi

echo
echo "m4_threads_wall_check_smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

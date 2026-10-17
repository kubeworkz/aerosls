#!/usr/bin/env bash
# linux_personality_check_smoke.sh — teeth for tests/linux_personality_check.sh.
#
# A guard nobody has watched fail is a guard nobody trusts, so every clause
# gets an arm that deletes or distorts exactly the thing it names and
# requires the guard to exit 1 SAYING WHICH. Each mutation is a mistake that
# could plausibly be made by a future edit (a store "cleaned up" as dead
# code, a predicate "simplified", a gate factored away), not an artificial
# corruption — the point is that the guard, not the author, catches it.
#
# The mutations run on COPIES (seed() below): the real tree is never
# touched, and the final baseline run on the real tree doubles as the
# seed-fidelity check — if a copy could not carry the wiring faithfully,
# the mutated runs would pass and this file would say so.
#
# A smoke that cannot run proves nothing, so every failure here is a
# failure: non-zero exit, or a mutation the guard did not name.
#
# Run:  bash tests/linux_personality_check_smoke.sh
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

GUARD=tests/linux_personality_check.sh
FILES=(
    kernel/linux_abi.h
    kernel/linux_abi.c
    kernel/process.h
    kernel/process.c
    kernel/cap.h
    kernel/cap.c
    kernel/syscall_dispatch.c
    arch/x86/user_paging.h
    arch/x86/syscall.asm
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

run_guard() { LINUX_PERSONALITY_CHECK_ROOT="$1" bash "$GUARD" 2>&1; }

pass=0
fail=0

# tooth <name> <clause> <description> <dir> — the guard must exit 1 and the
# output must name <clause>. Anything else is a smoke failure: a guard that
# stays green under its own mutation is the vacuous pass this file exists to
# refuse.
tooth() {
    local name="$1" clause="$2" desc="$3" d="$4" out rc
    out="$(run_guard "$d")"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "FAIL: $clause\."; then
        echo "TOOTH OK   $name — guard exited 1 naming $clause"
        pass=$((pass+1))
    else
        echo "TOOTH FAIL $name — guard rc=$rc (want 1), clause '$clause' $(printf '%s\n' "$out" | grep -c "FAIL: $clause\.") time(s)"
        printf '%s\n' "$out" | grep "FAIL:" | head -4 | sed 's/^/           /'
        fail=$((fail+1))
    fi
}

echo "linux_personality_check_smoke — teeth for the E7 increment-2 wiring guard"
echo "========================================================================"
echo

# ─── Baseline: the unmutated copy passes (vacuity control) ────────────────
seed "$W/base"
if out="$(run_guard "$W/base")" && [ $? -eq 0 ]; then
    echo "ok:   baseline — the seeded copy passes the guard unmutated"
else
    echo "ABORT: baseline — the seeded copy does not pass the guard unmutated:"
    printf '%s\n' "$out" | grep "FAIL:" | head -5 | sed 's/^/       /'
    exit 2
fi
echo

# ─── Teeth ────────────────────────────────────────────────────────────────

# C: the capture store deleted — "dead code" is how a six-instruction
# sequence disappears; the arguments are then destroyed before C reads them.
seed "$W/t1"
sed -i '/mov \[gs:0x50\], rdi/d' "$W/t1/arch/x86/syscall.asm"
tooth capture-removed C "one of the six argument captures is deleted" "$W/t1"

# C: a capture redirected onto a FROZEN syscall_regs offset — the desync
# static assert B1 cannot catch because it lives in the asm.
seed "$W/t2"
sed -i 's/mov \[gs:0x78\], r9/mov [gs:0x48], r9/' "$W/t2/arch/x86/syscall.asm"
tooth capture-wrong-offset C "a capture store lands on syscall_regs' frozen [gs:0x48]" "$W/t2"

# B1: the static assert deleted — a C-side reorder would then be silent.
seed "$W/t3"
sed -i '/offsetof(struct PerCPUData, linux_args) == 0x50/d' "$W/t3/arch/x86/user_paging.h"
tooth static-assert-dropped B1 "the linux_args frozen-offset assert is gone" "$W/t3"

# D: the predicate inverted to SLS — §10.3's decisive tooth at source
# level: SLS tasks would take the Linux branch and Linux tasks the chains.
seed "$W/t4"
sed -i 's/linux_personality_intercepts(cur->personality)/linux_personality_intercepts(PROC_PERSONALITY_SLS)/' "$W/t4/kernel/syscall_dispatch.c"
tooth gate-inverted D "the branch now claims SLS tasks instead of Linux ones" "$W/t4"

# D: the branch deleted outright — the number collision unguarded.
seed "$W/t5"
sed -i 's/if (cur \&\& linux_personality_intercepts(cur->personality)) {/if (0) {/' "$W/t5/kernel/syscall_dispatch.c"
tooth branch-removed D "the personality branch is dead-coded away" "$W/t5"

# F1: the spawn syscall passes SLS — a Linux-named spawn creating a plain
# SLS task; the mismatch would be silent until the task's first trap.
seed "$W/t6"
python3 - "$W/t6/kernel/cap.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = "                                  &ch_r, req->target_partition,\n                                  PROC_PERSONALITY_LINUX);"
new = "                                  &ch_r, req->target_partition,\n                                  PROC_PERSONALITY_SLS);"
assert old in s, "anchor not found"
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
tooth spawn-personality-dropped F1 "sys_sls_linux_spawn now passes PROC_PERSONALITY_SLS" "$W/t6"

# E: the slot reset deleted — the next occupant inherits a personality.
seed "$W/t7"
sed -i '/pd->personality = PROC_PERSONALITY_SLS;/d' "$W/t7/kernel/process.c"
tooth clear-dropped E "the reused-slot personality reset is gone" "$W/t7"

# F2: the E2 authority gate neutralised — any process could spawn tasks.
seed "$W/t8"
sed -i 's/if (!parent->sidecar_authority) {/if (0) {/' "$W/t8/kernel/cap.c"
tooth authority-gate-off F2 "the sidecar_authority gate is neutralised" "$W/t8"

echo
# ─── Restore check: the real tree was never touched ───────────────────────
if out="$(LINUX_PERSONALITY_CHECK_ROOT= bash "$GUARD")" && [ $? -eq 0 ]; then
    echo "RESTORE OK — the REAL tree still passes the guard (every mutation lived in a copy)"
    pass=$((pass+1))
else
    echo "RESTORE FAIL — the real tree no longer passes the guard:"
    printf '%s\n' "$out" | grep "FAIL:" | head -5 | sed 's/^/           /'
    fail=$((fail+1))
fi

echo
echo "linux_personality_check_smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

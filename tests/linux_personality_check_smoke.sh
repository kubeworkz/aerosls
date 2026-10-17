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
    kernel/chan.c
    arch/x86/user_paging.h
    arch/x86/syscall.asm
    user/sidecar/src/linux.rs
    user/sidecar/src/entry.rs
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

echo "linux_personality_check_smoke — teeth for the E7 increment-2 wiring"
echo "and increment-3 delivery guard"
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

# ─── Increment-3 teeth (§3.3 steps 5-7) ─────────────────────────────────

# I1: the post deleted — the task parks for a reply nobody will ever be
# asked for; the park machinery stays healthy, so nothing else notices.
seed "$W/t9"
sed -i '/r = cap_send_msg(pid, ch_w, req, (uint32_t)sizeof \*req, NULL, 0, 0, 0);/d' "$W/t9/kernel/chan.c"
tooth forward-post-removed I1 "the post before the park is deleted" "$W/t9"

# I2: the arm never set — cap_maybe_handoff then does a send-handoff whose
# sysret returns to ring-3 MID-forward with its hardcoded rax=0.
seed "$W/t10"
sed -i '/cur->park_syscall = SYS_LINUX_TRAP_RESUME;/d' "$W/t10/kernel/syscall_dispatch.c"
tooth forward-arm-missing I2 "the arm-before-post stamp is gone" "$W/t10"

# I3: the in-flight check inverted — now a stray ring-3 trap wearing the
# resume number takes the reply arm, and an armed forward misses its resume.
seed "$W/t11"
sed -i 's/if (!linux_forward_resume_is_live(/if (linux_forward_resume_is_live(/' "$W/t11/kernel/syscall_dispatch.c"
tooth resume-gate-inverted I3 "the resume arm no longer checks the in-flight state" "$W/t11"

# J1: the reply status returned raw — the sidecar's CAP_ENOSYS (-10) lands
# in rax as a CAP code no errno can produce (§5.7).
seed "$W/t12"
sed -i 's/return (uint64_t)(int64_t)linux_errno_from_cap(req->status);/return req->status;/' "$W/t12/kernel/linux_abi.c"
tooth reply-status-unmapped J1 "apply_reply no longer maps the status through §5.7" "$W/t12"

# J2: the echo check dead-coded — "no answer that was not an answer" is
# the whole of §5.7 one level up.
seed "$W/t13"
sed -i 's/if (req->num != expect)/if (0)/' "$W/t13/kernel/linux_abi.c"
tooth echo-check-dropped J2 "the num-echo refusal is gone from apply_reply" "$W/t13"

# K: the service call unhooked — requests stay queued while their task
# parks forever, and the wait set never learns to wake the sidecar.
seed "$W/t14"
sed -i 's/crate::linux::service(&RealKernel);//' "$W/t14/user/sidecar/src/entry.rs"
tooth service-unhooked K "the entry loop no longer services the Linux children" "$W/t14"

# K1: the default arm becomes a silent success — §11 Q5's forbidden answer
# to an unimplemented syscall.
seed "$W/t15"
sed -i 's/_ => Answer::Reply(0, E_NOSYS)/_ => Answer::Reply(0, STATUS_OK)/' "$W/t15/user/sidecar/src/linux.rs"
tooth default-arm-success K1 "the default arm now fakes a success instead of refusing ENOSYS" "$W/t15"

# K2: the wire size drifts one byte — the compile-time assert is the last
# thing between a layout change and an eternity of -EPROTO.
seed "$W/t16"
sed -i 's/pub const LINUX_SYSCALL_REQ_BYTES: usize = 72;/pub const LINUX_SYSCALL_REQ_BYTES: usize = 71;/' "$W/t16/user/sidecar/src/linux.rs"
tooth wire-size-drift K2 "the 72-byte pin moved to 71" "$W/t16"

# K3: the num echo never written — the kernel compares the echo against the
# first trap's number and would refuse every reply -EPROTO.
seed "$W/t17"
sed -i 's/payload\[0\.\.8\]\.copy_from_slice(\&num\.to_le_bytes());//' "$W/t17/user/sidecar/src/linux.rs"
tooth reply-missing-num-echo K3 "the reply no longer fills the echoed num field" "$W/t17"

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

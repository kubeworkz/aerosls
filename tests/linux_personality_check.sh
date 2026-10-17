#!/usr/bin/env bash
# linux_personality_check.sh — E7 increments 2+3 (Linux ABI Shim Design
# §3.3–§3.5): the personality branch, its capture and its spawn gate are
# WIRED, in the right places, in the right order — and so is increment 3's
# delivery: the post-then-park forward, the arm-before-post resume, the
# §5.7 errno map's exits, and the sidecar core's first answers.
#
# Increments 2 and 3 are one guard because they are one branch: clauses A–H
# hold that a Linux trap is described and refused (increment 2), clauses
# I–K hold that it is delivered, parked, resumed and answered (increment 3).
# Separately they could each be green while the seam between them is broken
# — an arm-before-post that refused correctly, a reply that mapped correctly
# into nobody's rax.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# Increment 2's whole deliverable is routing: a Linux task's trap must
# describe-and-refuse BEFORE any number chain, from arguments captured
# BEFORE any call, and the only thing that can spawn such a task must be
# the sidecar creator tree. None of that is observable without a boot —
# there is no qemu on a source-only runner, and the branch's absence is
# silent until a Linux binary's setuid reaches SYS_SLS_ALLOCATE. So the
# wiring itself is the artifact, and this guard holds each load-bearing
# property of it against the source: the sibling per-CPU block at the
# offsets the asm addresses literally, the six stores above every call,
# the branch's PREDICATE before the dispatcher's first switch, the slot
# reset that keeps a reused slot from inheriting a personality, and the
# spawn path that stamps LINUX only through the E2 authority gate.
#
# Every clause is one way the routing could be silently wrong, and the
# clause names the consequence, not just the pattern. The teeth for each
# live in tests/linux_personality_check_smoke.sh.
#
# Source-only: no build, no boot, no toolchain — so, like
# script_conventions_check.sh, it carries no GUARD-KIND marker: there is
# nothing it can legitimately be unable to do on a Linux runner.
#
# Run:  bash tests/linux_personality_check.sh
#       LINUX_PERSONALITY_CHECK_ROOT=<dir> to check a seeded tree (smoke).
set -uo pipefail

ROOT="${LINUX_PERSONALITY_CHECK_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 2

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails+1)); }

# ─── A. Preconditions ──────────────────────────────────────────────────────
for f in kernel/linux_abi.h kernel/linux_abi.c kernel/process.h kernel/process.c \
         kernel/cap.h kernel/cap.c kernel/syscall_dispatch.c kernel/chan.c \
         arch/x86/user_paging.h arch/x86/syscall.asm \
         user/sidecar/src/linux.rs user/sidecar/src/entry.rs; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f missing — the guard cannot be evaluated at all." >&2
        exit 2
    fi
done
ok "A. every file this guard reads exists"

UPH=arch/x86/user_paging.h
ASM=arch/x86/syscall.asm
PDH=kernel/process.h
PDC=kernel/process.c
CAPC=kernel/cap.c
CAPH=kernel/cap.h
DISP=kernel/syscall_dispatch.c
LABH=kernel/linux_abi.h
LABC=kernel/linux_abi.c
CANC=kernel/chan.c
LRS=user/sidecar/src/linux.rs
LRE=user/sidecar/src/entry.rs

# ─── B. The sibling per-CPU block, at the offsets the asm addresses ────────
# The struct side is pinned by static asserts (a C reorder is a BUILD
# error); this clause pins that those asserts still EXIST and the block is
# still six u64s — deleting an assert turns a build error back into a
# silent desync with the asm's literals.
if grep -qE 'uint64_t +linux_args\[6\];' "$UPH"; then
    ok "B. PerCPUData declares the six-register linux_args sibling block"
else
    bad "B. PerCPUData has no linux_args[6] — the Linux arguments have nowhere to live and the asm stores would write past the struct"
fi
if grep -q 'offsetof(struct PerCPUData, syscall_regs) == 0x10' "$UPH" && \
   grep -q 'offsetof(struct PerCPUData, linux_args) == 0x50' "$UPH" && \
   grep -q 'sizeof(struct PerCPUData) == 0x80' "$UPH"; then
    ok "B1. the frozen-offset static asserts pin syscall_regs@0x10, linux_args@0x50, size 0x80"
else
    bad "B1. a frozen-offset static assert is gone — a C-side reorder of PerCPUData would silently desync from syscall.asm's [gs:0xNN] literals"
fi

# ─── C. The capture: six exact stores, before ANY call ────────────────────
# §3.4's rule, with a live failure behind it: a late read of the entry
# frame captured garbage and resumed processes sysret'd into nothing. The
# registers are the Linux argument registers in order, at the exact
# offsets clause B pins.
capture_ok=1
capture_desc=""
i=0
for pair in "0x50 rdi" "0x58 rsi" "0x60 rdx" "0x68 r10" "0x70 r8" "0x78 r9"; do
    set -- $pair
    if ! grep -qE "mov +\[gs:$1\], +$2" "$ASM"; then
        capture_ok=0
        capture_desc="$capture_desc [$1]=$2"
    fi
    i=$((i+1))
done
if [ "$capture_ok" -eq 1 ]; then
    ok "C. syscall.asm captures all six Linux argument registers at [gs:0x50..0x80) in order"
else
    bad "C. syscall.asm is missing capture(s):$capture_desc — those arguments are destroyed by the entry path before C can read them, so the describe would be built from garbage"
fi

first_call_line=$(grep -nE '^[[:space:]]*call[[:space:]]' "$ASM" | head -1 | cut -d: -f1)
last_store_line=$(grep -nE 'mov +\[gs:0x(50|58|60|68|70|78)\]' "$ASM" | tail -1 | cut -d: -f1)
if [ -n "$first_call_line" ] && [ -n "$last_store_line" ] && \
   [ "$last_store_line" -lt "$first_call_line" ]; then
    ok "C1. every capture store sits above the stub's first call (the §3.4 before-any-call rule)"
else
    bad "C1. a capture store runs AFTER a call — the -O2 call chain may reuse the entry frame, and a late read is the exact live failure syscall_regs' own comment records"
fi

# ─── D. The branch: the predicate, before the dispatcher's first switch ────
# §10.3's decisive arm inverts this predicate; here the SOURCE must show
# it exists, names LINUX, and is consulted before any number chain.
if grep -q 'linux_personality_intercepts(cur->personality)' "$DISP"; then
    ok "D. do_syscall consults linux_personality_intercepts(cur->personality)"
else
    bad "D. do_syscall has no personality predicate — every trap takes the number chains, and a Linux task's rax=105 IS SYS_SLS_ALLOCATE (§2.2's collision, unguarded)"
fi
pred_line=$(grep -n 'linux_personality_intercepts(cur->personality)' "$DISP" | head -1 | cut -d: -f1)
first_switch_line=$(grep -n 'switch (num)' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$pred_line" ] && [ -n "$first_switch_line" ] && [ "$pred_line" -lt "$first_switch_line" ]; then
    ok "D1. the predicate is consulted BEFORE the dispatcher's first switch(num)"
else
    bad "D1. the personality branch sits after a number chain — an SLS arm would already have interpreted the Linux number before the branch could refuse it"
fi
if grep -q -- '-LINUX_ENOSYS' "$DISP"; then
    ok "D2. the refusal is -LINUX_ENOSYS (a Linux errno — the units this task's binary was compiled against), not a CAP-family value"
else
    bad "D2. the branch no longer refuses with -LINUX_ENOSYS — a CAP_E* negative in a Linux task's error path is a value no errno can produce (§5.7)"
fi

# ─── E. The slot reset: a reused slot must not inherit a personality ──────
if grep -q 'pd->personality = PROC_PERSONALITY_SLS;' "$PDC"; then
    ok "E. proc_clear_resume_state() resets personality to SLS (spawn init AND every teardown path)"
else
    bad "E. the personality reset is gone from process.c — the next occupant of a slot could trap through the previous occupant's branch"
fi
if grep -q '%%gs:0x50' "$PDC" && grep -q 'proc_read_linux_args' "$PDH"; then
    ok "E1. proc_read_linux_args() reads the sibling block ([%%gs:0x50]) and is declared in process.h"
else
    bad "E1. the C-side capture read is missing or undeclared — the branch would describe arguments it never read"
fi
if grep -qE 'uint32_t[[:space:]]+personality;' "$PDH"; then
    ok "E2. ProcessDescriptor carries the personality field"
else
    bad "E2. ProcessDescriptor has no personality field — there is no state for the branch to consult"
fi

# ─── F. The spawn gate: LINUX only through the E2 authority path ──────────
if grep -q 'pd->personality = personality;' "$CAPC"; then
    ok "F. the create stamps the child's personality (inside cap_create_sidecar_in_personality)"
else
    bad "F. the create no longer stamps the child — SYS_SLS_LINUX_SPAWN could succeed with an SLS task, and the mismatch would be silent until the task's first trap"
fi
if grep -A12 'sys_sls_linux_spawn' "$CAPC" | grep -q 'PROC_PERSONALITY_LINUX'; then
    ok "F1. sys_sls_linux_spawn passes PROC_PERSONALITY_LINUX to the create"
else
    bad "F1. sys_sls_linux_spawn does not pass PROC_PERSONALITY_LINUX — the spawn syscall would create a plain SLS task under a Linux name"
fi
if grep -q 'parent->sidecar_authority' "$CAPC"; then
    ok "F2. the E2 sidecar_authority gate is still inside the create both spawn syscalls share"
else
    bad "F2. the sidecar_authority gate is gone from cap.c — ANY ring-3 process could spawn tasks (and, through 328, Linux-personality ones) with crafted manifests (E2's confinement break)"
fi
if grep -q '#define SYS_SLS_LINUX_SPAWN 328' "$CAPH"; then
    ok "F3. cap.h defines SYS_SLS_LINUX_SPAWN as 328"
else
    bad "F3. SYS_SLS_LINUX_SPAWN is not defined as 328 — the syscall the sidecar will call does not exist or has drifted"
fi
if grep -q 'case SYS_SLS_LINUX_SPAWN:' "$DISP" && \
   grep -A2 'case SYS_SLS_LINUX_SPAWN:' "$DISP" | grep -q 'sys_sls_linux_spawn'; then
    ok "F4. the dispatcher routes 328 to sys_sls_linux_spawn"
else
    bad "F4. case SYS_SLS_LINUX_SPAWN is absent or unrouted — the syscall number falls into default"
fi

# ─── G. The number space: 328 is unique across every SYS_SLS_* define ─────
# python3 is an instrument, not a property: absent, the scan cannot happen
# and that is an ABORT (a skip), never a pass and never a red property.
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not available — the SYS_SLS_* number-space scan cannot run." >&2
    exit 2
}
python3 - <<'PY'
import re, sys, glob
nums = {}
dupes = []
for f in glob.glob('kernel/*.h') + glob.glob('net/*.h') + glob.glob('drivers/*.h'):
    for m in re.finditer(r'#define\s+(SYS_SLS_\w+)\s+(\d+)', open(f, encoding='utf-8', errors='replace').read()):
        name, num = m.group(1), int(m.group(2))
        if num in nums:
            # The SAME name in a second header is a re-declaration of one
            # syscall (e.g. a shared prototype header) — not a collision.
            # Two DIFFERENT names on one number is the real ambiguity: the
            # dispatcher could not tell the two syscalls apart.
            if nums[num] != name:
                dupes.append((num, nums[num], name))
        else:
            nums[num] = name
if dupes:
    for num, a, b in dupes:
        print(f"FAIL: G. syscall number {num} is defined twice: {a} and {b}")
    sys.exit(1)
if nums.get(328) != 'SYS_SLS_LINUX_SPAWN':
    print(f"FAIL: G. 328 belongs to {nums.get(328)}, not SYS_SLS_LINUX_SPAWN")
    sys.exit(1)
print("ok:   G. every SYS_SLS_* number is unique, and 328 is SYS_SLS_LINUX_SPAWN")
sys.exit(0)
PY
g_rc=$?
[ "$g_rc" -eq 0 ] || fails=$((fails+1))

# ─── H. The contracts the branch consumes exist as contracts ──────────────
if grep -q 'struct SLSLinuxSyscallRequest' "$LABH" && \
   grep -q 'linux_syscall_describe' "$LABH" && \
   grep -qE '#define PROC_PERSONALITY_SLS[[:space:]]+0' "$LABH"; then
    ok "H. linux_abi.h carries the request struct, describe(), and SLS==0"
else
    bad "H. a contract the branch consumes is gone from linux_abi.h — the request's ABI (increment 3's channel post) is no longer pinned anywhere"
fi

# ─── I. The delivery: post-then-park, arm-before-post, resume-by-arm ───────
# Increment 3's wiring, one consequence per clause: a park nobody wakes, a
# sysret handoff returned MID-forward (its hardcoded rax=0 loses the reply),
# a raw ring-3 trap reading a reply nobody owed — each is silent in the
# source until a Linux binary hits it, which no runner here can do.
if grep -q 'int k_linux_forward(' "$CANC" && grep -q 'uint64_t k_linux_deliver(' "$CANC"; then
    ok "I. chan.c carries the forward/deliver pair — §3.3 steps 5-7's channel path exists at all"
else
    bad "I. the k_linux_forward/k_linux_deliver pair is gone from chan.c — a Linux trap has no route to its sidecar and no path back"
fi

# Scoped to the forward's own body: k_chan_send parks on the same shape
# (line ~447) for its own send, and that park has nothing to do with order
# inside this function.
fwd_start=$(grep -n 'int k_linux_forward(' "$CANC" | head -1 | cut -d: -f1)
send_line=$(grep -n 'cap_send_msg(pid, ch_w, req' "$CANC" | head -1 | cut -d: -f1)
park_line=$(grep -n 'cap_wait_chans(&chan_id, 1,' "$CANC" | awk -F: -v s="$fwd_start" '$1 > s { print $1; exit }')
if [ -n "$fwd_start" ] && [ -n "$send_line" ] && [ -n "$park_line" ] && \
   [ "$send_line" -gt "$fwd_start" ] && [ "$send_line" -lt "$park_line" ]; then
    ok "I1. k_linux_forward posts the 72-byte request BEFORE it parks"
else
    bad "I1. the post does not precede the park inside k_linux_forward — the task would park for a reply to a request that was never sent, and nothing wakes it"
fi

arm_line=$(grep -n 'cur->park_syscall = SYS_LINUX_TRAP_RESUME;' "$DISP" | head -1 | cut -d: -f1)
fwd_call=$(grep -n 'k_linux_forward(' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$arm_line" ] && [ -n "$fwd_call" ] && [ "$arm_line" -lt "$fwd_call" ] && \
   [ "$(grep -c 'cur->park_syscall = 0;' "$DISP")" -ge 2 ]; then
    ok "I2. park_syscall is armed BEFORE the post and cleared on the refusal paths (cap_maybe_handoff skips the send-handoff while armed)"
else
    bad "I2. the arm-before-post ordering or the un-arm is gone — cap_maybe_handoff would sysret to ring-3 MID-forward with its hardcoded rax=0 and the reply is lost"
fi

if grep -q 'if (!linux_forward_resume_is_live(' "$DISP" && \
   grep -q 'k_linux_deliver(cur->pid, ch_r' "$DISP"; then
    ok "I3. the resume arm checks the in-flight state before it delivers — a stray sentinel is refused by name"
else
    bad "I3. the in-flight check before k_linux_deliver is gone — a raw ring-3 trap wearing the resume number could read (or park for) a reply that was never owed"
fi

# ─── J. The map: a refusal on the delivery path leaves as a Linux errno ────
# §5.7 at both exits of the exchange: the FIRST trap's refusals (captured
# truth -> errno), and the sidecar's reply status (CAP units -> Linux units),
# the last place either unit is available.
if grep -q 'linux_errno_from_cap(status)' "$DISP" && grep -q 'CAP_ERR_REVOKED' "$DISP"; then
    ok "J. both forward-failure exits (post refused, sidecar gone) cross linux_errno_from_cap — no raw CAP value ever reaches rax"
else
    bad "J. a forward-path refusal no longer crosses linux_errno_from_cap — a CAP-family negative in a Linux task's rax is a value no errno can produce (§5.7), and a dead sidecar would surface as a debug code instead of -EIO"
fi
if grep -q 'linux_errno_from_cap(req->status);' "$LABC"; then
    ok "J1. linux_syscall_apply_reply maps a nonzero reply status through the table before returning"
else
    bad "J1. the reply's status is no longer mapped in apply_reply — the sidecar's CAP_ENOSYS (-10) would land in rax as a raw CAP code"
fi
if grep -q 'payload_len != sizeof \*req' "$LABC" && grep -q 'req->num != expect' "$LABC"; then
    ok "J2. apply_reply refuses by size and by num echo BEFORE trusting the payload (72 bytes, this request's number)"
else
    bad "J2. the size or echo check is gone from apply_reply — a malformed or foreign payload could put an unvalidated value into the task's rax (§5.7's 'no answer that was not an answer')"
fi

# ─── K. The sidecar's first answers (user/sidecar/src/linux.rs, entry.rs) ──
# Step 6 lives in Rust, and none of it is visible from a kernel clause: the
# entry loop must actually call the service arm, the answers must be the
# census's honest set (§11 Q5), and the reply must be the exact struct the
# kernel's apply will accept or every exchange dies -EPROTO.
if grep -qF 'crate::linux::service(&RealKernel);' "$LRE" && \
   grep -qF 'crate::linux::append_wait(' "$LRE"; then
    ok "K. the entry loop services the Linux children AND adds their channels to the quiesce wait set (a forward wakes the sidecar, not an idle pass)"
else
    bad "K. the entry loop no longer services or wait-qualifies the Linux children — every forwarded syscall sits unanswered in the channel queue while its task parks forever"
fi
if grep -qF '218 => Answer::Reply(args[0], STATUS_OK),' "$LRS" && \
   grep -qF '231 => Answer::Exit,' "$LRS" && \
   grep -qF '_ => Answer::Reply(0, E_NOSYS),' "$LRS"; then
    ok "K1. the answers are the census's: set_tid_address echoes, exit_group reaps, everything else refuses ENOSYS with its number logged"
else
    bad "K1. an answer arm changed shape — worst is the default arm: a silent success for an unimplemented syscall is the answer §11 Q5's policy forbids"
fi
if grep -qF 'pub const LINUX_SYSCALL_REQ_BYTES: usize = 72;' "$LRS" && \
   grep -qF 'size_of::<LinuxSyscallReq>() == LINUX_SYSCALL_REQ_BYTES' "$LRS"; then
    ok "K2. the 72-byte wire size is pinned twice: a literal and a compile-time assert"
else
    bad "K2. the wire-size pin is gone — a kernel/sidecar layout drift makes every reply unparseable, and the kernel's apply then refuses every one -EPROTO forever"
fi
if grep -qF 'payload[0..8].copy_from_slice(&num.to_le_bytes());' "$LRS" && \
   grep -qF 'payload[64..68].copy_from_slice(&status.to_le_bytes());' "$LRS"; then
    ok "K3. the reply fills num, ret and status at the pinned offsets — the fields linux_syscall_apply_reply compares against"
else
    bad "K3. the reply no longer fills num/status at the pinned offsets — every reply would fail the kernel's echo check and the task could only ever see -EPROTO"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "linux_personality_check: all clauses hold"
    exit 0
fi
echo "linux_personality_check: $fails clause(s) failed"
exit 1

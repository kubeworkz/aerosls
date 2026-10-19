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
# Clause set L (§5.3, the plan's increment 4) joins them because the one-MSR
# seam spans the same files: field cleared at slot reuse, staged at the first
# arm, applied at the reply through the canonicality re-check, loaded at every
# switch-in — four places where a silently-wrong TLS base is invisible to
# every other clause here.
# Clause set M (the plan's increment 5 — the fd class: "the kernel copy plus
# the sidecar's fd table") holds the byte crossing: the plan's clamp riding
# IN the description, the copy refusing before the arm, the forward carrying
# attached bytes within the channel's payload bound, and the sidecar's fd
# table answering EBADF — every one of them a place where the crossing is
# silently truncated, oversized, or performed against a descriptor that was
# never checked.
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
         arch/x86/user_paging.h arch/x86/user_paging.c arch/x86/syscall.asm \
         user/sidecar/src/linux.rs user/sidecar/src/entry.rs \
         user/sidecar/src/e7_probe.rs user/sidecar/fixtures/e7_linux_probe.S; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f missing — the guard cannot be evaluated at all." >&2
        exit 2
    fi
done
ok "A. every file this guard reads exists"

UPH=arch/x86/user_paging.h
UPC=arch/x86/user_paging.c
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
LPRO=user/sidecar/src/e7_probe.rs
FIX=user/sidecar/fixtures/e7_linux_probe.S
E64H=kernel/elf64_load.h
E64C=kernel/elf64_load.c
CAND=user/sidecar/src/e7_candidate.rs
CBUILD=user/sidecar/fixtures/build_candidate.sh

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
send_line=$(grep -n 'cap_send_msg(pid, ch_w, msg' "$CANC" | head -1 | cut -d: -f1)
park_line=$(grep -n 'cap_wait_chans(&chan_id, 1,' "$CANC" | awk -F: -v s="$fwd_start" '$1 > s { print $1; exit }')
if [ -n "$fwd_start" ] && [ -n "$send_line" ] && [ -n "$park_line" ] && \
   [ "$send_line" -gt "$fwd_start" ] && [ "$send_line" -lt "$park_line" ]; then
    ok "I1. k_linux_forward posts the message (the 72-byte description plus any attached fd-class bytes) BEFORE it parks"
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
if grep -qF 'crate::linux::service(&RealKernel, &booted.console);' "$LRE" && \
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

# ─── L. §5.3 (the plan's increment 4): the one MSR, both sides of its seam ──
# arch_prctl(ARCH_SET_FS) is the single Linux semantic the KERNEL owns, and
# every way it is silently wrong is invisible without a boot: a field never
# cleared (a reused slot's stale TLS handed onward), a switch path that stops
# loading it (two tasks sharing one base), an apply that trusts the grant's
# address (the dangerous half unvalidated), a policy arm that grants more
# than SET_FS at a non-canonical value, or a bound rewritten until it admits
# everything. The host test bites the two pure functions; these clauses hold
# the four call sites the host test cannot see.
if grep -q '#define MSR_FS_BASE' "$UPH" && \
   grep -q 'void user_write_fs_base(uint64_t base);' "$UPH" && \
   grep -q 'void user_write_fs_base(uint64_t base)' "$UPC" && \
   grep -q 'wrmsr(MSR_FS_BASE, base)' "$UPC" && \
   [ "$(grep -c 'wrmsr(MSR_FS_BASE' "$UPC")" -eq 2 ]; then
    ok "L. user_write_fs_base exists once — declared beside the gate, implemented over MSR_FS_BASE (the tree's ONLY writer, plus the gate's own zero)"
else
    bad "L. user_write_fs_base is missing, non-functional, or MSR_FS_BASE is written elsewhere too — §5.3's one kernel semantic has no single writer, so the invariant 'FSBASE == current task's fs_base' has nothing maintaining it"
fi
if grep -q 'wrmsr(MSR_FS_BASE, 0)' "$UPC"; then
    ok "L1. gate init zeroes MSR_FS_BASE before any ring-3 task exists — the invariant is TRUE from the first ring-3 instruction, not established later"
else
    bad "L1. the gate no longer zeroes the FS base — the first scheduled task's %fs starts at firmware's leftover value, and every later write plays catch-up with an unknown baseline"
fi
if grep -qE 'uint64_t +fs_base;' "$PDH" && \
   grep -q 'pd->fs_base = 0;' "$PDC" && \
   grep -q 'pd->fs_stage_valid = 0;' "$PDC"; then
    ok "L2. ProcessDescriptor carries fs_base, and the slot-reuse clear zeroes it AND its staged state beside personality"
else
    bad "L2. the field or its slot-reuse clearing is gone — the next occupant of a reused slot is scheduled with its predecessor's TLS base live in the MSR, pointing into someone else's address space"
fi
if grep -q 'user_write_fs_base(next->fs_base);' "$PDC"; then
    ok "L3. every ring-3 switch-in loads FSBASE from the incoming task's field (proc_build_resume_frame — the single frame builder every switch honours)"
else
    bad "L3. the switch-in no longer writes FSBASE — two Linux tasks running back to back silently share the first one's TLS base, with nothing observable until a %fs walk reads the wrong task's data"
fi
stage_line=$(grep -n 'cur->fs_stage_valid = 1;' "$DISP" | head -1 | cut -d: -f1)
arm_line53=$(grep -n 'cur->park_syscall = SYS_LINUX_TRAP_RESUME;' "$DISP" | head -1 | cut -d: -f1)
deliver_line=$(grep -n 'k_linux_deliver(cur->pid, ch_r' "$DISP" | head -1 | cut -d: -f1)
apply_line=$(grep -n 'user_write_fs_base(cur->fs_base);' "$DISP" | head -1 | cut -d: -f1)
recheck_n=$(grep -c 'linux_fs_addr_is_user_canonical(' "$DISP")
if [ -n "$stage_line" ] && [ -n "$arm_line53" ] && [ -n "$deliver_line" ] && \
   [ -n "$apply_line" ] && [ "$stage_line" -lt "$arm_line53" ] && \
   [ "$apply_line" -gt "$deliver_line" ] && [ "$recheck_n" -ge 1 ] && \
   grep -q -- '-LINUX_EINVAL' "$DISP"; then
    ok "L4. the seam holds: staged BEFORE the arm, applied only AFTER the reply is delivered, through the canonicality re-check (a granted SET_FS lands as a write; a bad address as -EINVAL)"
else
    bad "L4. the stage/apply seam is broken — no stage before the arm, a write that does not follow the reply, or the wrmsr no longer re-checks canonicality: the sidecar's grant alone would decide what reaches the MSR"
fi
if grep -q 'linux_arch_prctl_is_set_fs(uint64_t num, uint64_t cmd)' "$LABH" && \
   grep -q 'int linux_fs_addr_is_user_canonical(uint64_t addr)' "$LABH" && \
   grep -q 'return num == LINUX_ARCH_PRCTL && cmd == LINUX_ARCH_SET_FS;' "$LABC" && \
   grep -q 'return addr <= LINUX_USER_ADDR_MAX;' "$LABC" && \
   grep -q '#define LINUX_USER_ADDR_MAX' "$LABH"; then
    ok "L5. both §5.3 bounds exist as the named pure contracts (one predicate at both seam ends, one unsigned canonicality compare the host test bites)"
else
    bad "L5. a §5.3 pure bound is gone or rewritten as something other than the named comparison — the host test's teeth bite these exact functions, and a non-pure replacement voids them while the guard's grep still passes"
fi
if grep -qF '158 => match (args[0], args[1]) {' "$LRS" && \
   grep -qF '(0x1002, a) if a <= 0x0000_7FFF_FFFF_FFFF => {' "$LRS" && \
   grep -qF '(0x1002, _) => Answer::Reply(0, E_INVAL),' "$LRS" && \
   grep -qF 'const E_INVAL: i32 = -1;' "$LRS"; then
    ok "L6. the sidecar's 158 arm grants ONLY SET_FS at a user-canonical address; a bad address is E_INVAL (a bad VALUE), everything else ENOSYS — CAP_EINVAL pinned at -1 for §5.7's map"
else
    bad "L6. the policy arm changed shape — a grant for a non-canonical address or a non-SET cmd, or a drifted E_INVAL constant (the map would then answer a different error than the policy meant)"
fi

# ─── M. The plan's increment 5 (the fd class): the kernel copy, the fd table ──
# write(2) is the one fd-class byte crossing: the bytes live in the CHILD's
# address space (§3.5), so the kernel copies them out and the sidecar's fd
# table performs them. Every clause below is a way that crossing is silently
# wrong while every other clause here stays green: a clamp that leaves the
# header claiming a count the payload does not carry, a copy attached AFTER
# the arm (or not at all), a chunk the channel's pool refuses only for big
# writes, a walker that reads what ring 3 could not, an fd table that never
# registers stdio, and an invariant the sidecar stops checking. The host test
# bites the plan and the copy's every branch (67 checks, no boot); these
# clauses hold the call sites, the arithmetic across two headers, and the
# Rust half the host test cannot see.
if grep -q 'struct LinuxFdWritePlan linux_fd_write_plan(uint64_t num,' "$LABH" && \
   grep -q 'struct LinuxFdWritePlan linux_fd_write_plan' "$LABC" && \
   grep -q '#define LINUX_WRITE' "$LABH" && \
   grep -q '#define LINUX_EFAULT' "$LABH" && \
   grep -q '#define LINUX_FD_WRITE_CHUNK' "$LABH"; then
    ok "M. the fd class's contracts exist: the pure plan, the write number, EFAULT as Linux's own literal, and the chunk"
else
    bad "M. an fd-class contract is gone from linux_abi.h/.c — the dispatcher's copy path has no plan to consult and no named errno to refuse with, so a bad span can only be discovered by faulting"
fi
chunk_n=$(grep -oE '#define LINUX_FD_WRITE_CHUNK +[0-9]+' "$LABH" | awk '{print $3}')
pay_n=$(grep -oE '#define CAP_MSG_MAX_PAYLOAD +[0-9]+' "$CAPH" | awk '{print $3}')
if [ -n "$chunk_n" ] && [ -n "$pay_n" ] && [ $((chunk_n + 72)) -le "$pay_n" ]; then
    ok "M1. the chunk plus the 72-byte header fits the channel payload bound (${chunk_n} + 72 <= ${pay_n}) — no write can post a message the pool refuses"
else
    bad "M1. LINUX_FD_WRITE_CHUNK (${chunk_n:-?}) plus the header exceeds CAP_MSG_MAX_PAYLOAD (${pay_n:-?}) — every write above the pool's bound would be refused at post time, so the crossing dies as an errno instead of happening"
fi
plan_l=$(grep -n 'plan = linux_fd_write_plan(num, largs);' "$DISP" | head -1 | cut -d: -f1)
desc_l=$(grep -n 'linux_syscall_describe(&req, num, largs);' "$DISP" | head -1 | cut -d: -f1)
copy_l=$(grep -n 'linux_user_copy_out(fwd +' "$DISP" | head -1 | cut -d: -f1)
arm_l5=$(grep -n 'cur->park_syscall = SYS_LINUX_TRAP_RESUME;' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$plan_l" ] && [ -n "$desc_l" ] && [ -n "$copy_l" ] && \
   [ -n "$arm_l5" ] && [ "$plan_l" -lt "$desc_l" ] && \
   [ "$copy_l" -lt "$arm_l5" ] && \
   grep -qF 'if (plan.len) largs[2] = plan.len;' "$DISP" && \
   grep -qF 'return (uint64_t)(int64_t)plan.refuse;' "$DISP" && \
   grep -qF 'return (uint64_t)(int64_t)(-LINUX_EFAULT);' "$DISP" && \
   grep -qF 'k_linux_forward(cur->pid, ch_w, ch_r, fwd, fwd_len);' "$DISP"; then
    ok "M2. the seam's order: plan (clamp IN the header, refusal before any channel) -> describe -> copy -> arm -> forward(fwd, len) — every refusal lands before the arm"
else
    bad "M2. the dispatcher's copy seam broke — no clamp in the description (header, payload and rax disagree), a copy after the arm (a refusal with an arm to strand), a copy never attached, or a forward still passing the bare &req"
fi
if grep -qF 'const void* msg, uint32_t msg_len' "$CANC" && \
   grep -qF 'r = cap_send_msg(pid, ch_w, msg, msg_len, NULL, 0, 0, 0);' "$CANC" && \
   grep -qF 'msg_len < sizeof(struct SLSLinuxSyscallRequest)' "$CANC" && \
   grep -qF 'const void* msg, uint32_t msg_len);' "$CAPH"; then
    ok "M3. k_linux_forward takes the message and its length end to end: posts header+bytes, refuses an undersize message as CAP_ERR_PROTO, prototype matches definition"
else
    bad "M3. the forward's message path drifted — an undersize message accepted, a send that drops the attached bytes (every write would cross as a description of bytes that are not there), or a prototype/definition mismatch"
fi
if grep -qF '!(e & LAB_PTE_USER)' "$LABC" && \
   grep -qF '#define LAB_PTE_PS' "$LABC" && \
   grep -qF '!(e & LAB_PTE_PRESENT)' "$LABC" && \
   [ "$(grep -c 'return -LINUX_EFAULT;' "$LABC")" -ge 4 ] && \
   ! grep -qE 'get_or_create|allocate_physical_ram_frame' "$LABC"; then
    ok "M4. the copy walks read-only with all three boundaries (present, user at every level, no huge page) and allocates NOTHING — a tenant pointer cannot exhaust the kernel or reach a supervisor page"
else
    bad "M4. the copy's boundaries are gone or it gained an allocator — a supervisor page or a huge stride could cross to the sidecar, or a crafted address could turn a read into a memory lever (walk_page_tables' own allocate is exactly what this walker must NOT be)"
fi
if grep -qF 'const E_BADF: i32 = -2;' "$LRS" && \
   grep -qF '1 => match fd_kind(fds, args[0]) {' "$LRS" && \
   grep -qF 'None => Answer::Reply(0, E_BADF),' "$LRS" && \
   grep -qF 'fds: [FdSlot; LINUX_MAX_FDS],' "$LRS" && \
   grep -qF 't.fds = [FdSlot { used: false, kind: 0 }; LINUX_MAX_FDS];' "$LRS" && \
   grep -qF 't.fds.iter_mut().take(3)' "$LRS"; then
    ok "M5. the sidecar's fd table exists per task, is born with console stdio (0/1/2), and an unknown descriptor is EBADF — CAP_EBADF pinned at -2 so §5.7 delivers 9"
else
    bad "M5. the fd table or its EBADF arm is gone — a descriptor nobody registered would be performed (or fake-answered) instead of refused, and a drifted E_BADF would surface a different errno than the policy meant"
fi
if grep -qF 'let want = LINUX_SYSCALL_REQ_BYTES + args[2] as usize;' "$LRS" && \
   grep -qF 'if args[2] as usize > FD_PAYLOAD_CAP || rr.len != want {' "$LRS" && \
   grep -qF 'const FD_PAYLOAD_CAP: usize = 1024;' "$LRS" && \
   grep -qF 'match console.write(&payload[..len]) {' "$LRS" && \
   grep -qF 'crate::linux::service(&RealKernel, &booted.console);' "$LRE"; then
    ok "M6. the sidecar checks the attached bytes against the kernel's own arithmetic (count == payload, chunk twin), performs them on the loop's console, and is handed that console"
else
    bad "M6. the payload invariant, the chunk twin, the console perform, or the service's console hand-off is gone — bytes the task did not write could be performed, or a write could never reach the serial log it exists to prove"
fi

# ─── N. The plan's increment 6 (the mem class): the budget span, the W^X refusal ──
# brk's row (§5.4) is a bump pointer inside a reserved span, and every way
# it is silently wrong is invisible without a boot: a span that is not the
# manifest's own, a stage that survives slot reuse, a grant mapped without
# the kernel's re-check against ITS span, a frame that is neither the
# partition's nor zeroed, a partial batch that leaks its frames into the
# pool, a refusal shape that lies to libc (Linux returns the CURRENT break
# on a failed brk, never a negative), and a W^X rule that exists at only
# one end of the seam. The host test bites the pure verdict, the predicate
# and the frame reader (23 new checks, no boot); these clauses hold the
# call sites, the span's two derivations, and the Rust half the host test
# cannot see.
if grep -q 'struct LinuxMemBrkVerdict' "$LABH" && \
   grep -q 'linux_mem_brk_verdict(uint64_t heap_base' "$LABH" && \
   grep -q 'struct LinuxMemBrkVerdict linux_mem_brk_verdict(' "$LABC" && \
   grep -q 'int linux_mprotect_refuses_wx(uint64_t prot)' "$LABH" && \
   grep -q 'int linux_mprotect_refuses_wx(uint64_t prot)' "$LABC" && \
   grep -q 'uint64_t linux_user_page_frame(uint64_t root, uint64_t va)' "$LABH" && \
   grep -q 'uint64_t linux_user_page_frame(uint64_t root, uint64_t va)' "$LABC"; then
    ok "N. the mem class's contracts exist: the pure brk verdict, the W|X predicate, the frame reader — each declared where the dispatcher and the host test both read them"
else
    bad "N. a mem-class contract is gone from linux_abi.h/.c — the brk apply has no verdict to consult (a grant would be trusted about the address it asks the kernel to map), the W|X refusal has no predicate, and the unwind has no read-back"
fi
if grep -q 'pd->linux_heap_base = 0;' "$PDC" && \
   grep -q 'pd->linux_heap_limit = 0;' "$PDC" && \
   grep -q 'pd->linux_brk_cur = 0;' "$PDC" && \
   grep -q 'pd->mem_stage_valid = 0;' "$PDC"; then
    ok "N1. proc_clear_resume_state() zeroes the heap span AND the staged brk — a reused slot inherits neither its predecessor's pages-to-be nor an unanswered grant"
else
    bad "N1. the mem state's slot-reuse clear is gone from process.c — the next occupant of a reused slot is handed its predecessor's brk_cur (whose grants map pages at a break it never asked for) and its live brk stage"
fi
span_line="$(grep -nF 'pd->linux_heap_base  = stack_base + (uint64_t)stk_pages * 4096;' "$CAPC" | cut -d: -f1)"
# The personality check is bound to the STAMP SITE (the line above the
# stamp), not to the bare pattern: increment 7 added other
# PROC_PERSONALITY_LINUX blocks to cap.c, and a free-standing pattern
# match let the N2 mutation sail through them — a vacuous pass.
if [ -n "$span_line" ] && \
   sed -n "$((span_line - 1))p" "$CAPC" | grep -qF 'if (personality == PROC_PERSONALITY_LINUX) {' && \
   grep -qF '((m.budget_mem_bytes + 4095ULL) / 4096ULL)' "$CAPC" && \
   grep -qF 'const IMAGE_VBASE: u64 = 0x4000_0000_0000;' "$LPRO" && \
   grep -qF 'const MEM_BUDGET_BYTES: u64 = 64 * 1024;' "$LPRO" && \
   grep -qF 'let heap_base = stack_base + stk_pages * 4096;' "$LPRO"; then
    ok "N2. the span is stamped inside the create, for LINUX only, from the manifest's BUDGET record — and the sidecar's ledger derives the same three numbers from the same record (one truth, mirrored)"
else
    bad "N2. the span's stamp or its twin is gone — the kernel would map brk pages against a span the manifest never declared (or stamp one onto every SLS slot), or the sidecar's ledger would be born at a base the kernel's pages never reach: the two halves silently disagree and every denial-echo check lies"
fi
if grep -qF 'linux_mprotect_refuses_wx(largs[2]))' "$DISP" && \
   grep -qF '(uint64_t)(int64_t)(-LINUX_EPERM);' "$DISP" && \
   grep -qF 'if args[2] & 0x2 != 0 && args[2] & 0x4 != 0 {' "$LRS" && \
   grep -qF 'const E_PERM: i32 = -12;' "$LRS"; then
    ok "N3. W|X is refused at BOTH ends: the first trap refuses -EPERM before any post, and the sidecar's arm carries the same refusal (CAP_EPERM pinned at -12 so §5.7 delivers 1)"
else
    bad "N3. the W|X refusal exists at one end or neither — one deletion away from a shim that maps write+execute pages, which is the exclusion H2 names (and E_PERM drifted would deliver a different errno than the refusal meant)"
fi
stage_line6=$(grep -n 'cur->mem_stage_valid = 1;' "$DISP" | head -1 | cut -d: -f1)
arm_line6=$(grep -n 'cur->park_syscall = SYS_LINUX_TRAP_RESUME;' "$DISP" | head -1 | cut -d: -f1)
verdict_line=$(grep -n 'linux_mem_brk_verdict(' "$DISP" | head -1 | cut -d: -f1)
deliver_line6=$(grep -n 'k_linux_deliver(cur->pid, ch_r' "$DISP" | head -1 | cut -d: -f1)
map_line=$(grep -n 'cur->linux_brk_cur = v.new_cur;' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$stage_line6" ] && [ -n "$arm_line6" ] && [ -n "$verdict_line" ] && \
   [ -n "$deliver_line6" ] && [ -n "$map_line" ] && \
   [ "$stage_line6" -lt "$arm_line6" ] && \
   [ "$verdict_line" -gt "$deliver_line6" ] && [ "$map_line" -gt "$verdict_line" ] && \
   grep -qF 'if (v.malformed) {' "$DISP" && \
   grep -qF 'cur->linux_brk_cur = v.new_cur;' "$DISP"; then
    ok "N4. the brk seam holds: staged BEFORE the arm, the verdict consulted only AFTER the reply is delivered, malformed grants refused before any map, and the break recorded only on a fully-mapped grant"
else
    bad "N4. the stage/apply seam is broken — a grant mapped without the verdict's span re-check (the reply would decide what the kernel maps), a stage that survives past its own reply, or a break that moves without pages behind it: a silent lie to ring-3 about its own heap"
fi
if grep -qF 'void *frame = allocate_physical_ram_frame_for_partition(cur->partition_id);' "$DISP" && \
   grep -qF 'w[zi] = 0;' "$DISP" && \
   grep -qF 'USER_PTE_NOEXEC);' "$DISP"; then
    ok "N5. granted pages are the PARTITION's own frames (quota-charged, the hard ceiling §5.4 names), zero-filled before the task can read them, mapped non-executable"
else
    bad "N5. the brk frames are no longer partition-charged, no longer zeroed, or executable — an unzeroed frame leaks its last tenant's bytes into a fresh heap (the pool recycles across partitions), and an uncharged frame bypasses the very ceiling §5.4 rests on"
fi
if grep -qF 'f = linux_user_page_frame(' "$DISP" && \
   grep -qF 'user_unmap_page(pml4, va);' "$DISP" && \
   grep -qF 'free_physical_ram_frame_for_partition(' "$DISP"; then
    ok "N6. a grant that runs out of frames mid-batch UNWINDS: the mapped frames are read back by the non-allocating walker, unmapped, and returned to the partition — no leak, no half-mapped grant"
else
    bad "N6. the mid-batch unwind is gone — a frame-exhausted brk leaks every frame it already mapped (the break stays but the partition's quota is spent on pages nobody owns), and the task's heap has holes the break claims are backed"
fi
if grep -qF 'if want > brk.cur && want <= brk.limit {' "$LRS" && \
   grep -qF 'Answer::Reply(brk.cur, STATUS_OK)' "$LRS" && \
   grep -qF 't.brk_cur = heap_base;' "$LRS"; then
    ok "N7. the sidecar's brk arm grants only inside the span and answers the UNCHANGED current break otherwise — Linux's own failure shape, a value not an errno, so no libc's brk wrapper can be surprised"
else
    bad "N7. the brk arm changed shape — an errno on refusal (no libc's brk wrapper is written against it), a grant outside the span (the kernel's verdict then refuses and the two halves disagree on every reply), or a ledger not born at the span's base"
fi
if grep -qF 'E7MEM-BRK-OK' "$FIX" && \
   [ "$(grep -cE 'mov +\$39[5-8], %eax' "$FIX")" -eq 4 ] && \
   grep -qF 'mov     $12, %eax' "$FIX" && \
   grep -qF 'mov     $10, %eax' "$FIX" && \
   grep -qF 'cmp     %r13, %rax' "$FIX"; then
    ok "N8. the probe fixture carries the mem block: brk query/grant/heap-write (the E7MEM positive marker), the denial-echo check, the W|X check, and four markers (395-398) the evidence asserts absent"
else
    bad "N8. the probe's mem block is gone or gutted — the shipped boot would prove nothing about brk while reading exactly as green (the 391/392 discipline's whole point: the markers' ABSENCE is the assertion)"
fi

# ─── O. The plan's increment 7 (the image class): the ONE ELF mapper, the §5.2 stack ──
# A real image format's load path has two halves that must not drift: the
# mapper's gates (§5.1 — ET_EXEC, linked vaddrs, the flags from the
# segment) and the create's stack image (§5.2 — child-vaddr pointers, the
# BIB's page kept out of it, entropy refused rather than substituted).
# Every clause below is a way a real ELF child is silently entered against
# garbage that the .flat probe CANNOT see — which is exactly why
# increment 7 exists. Teeth: clause set O in the smoke.
if grep -qF 'int elf_blob_has_magic(const uint8_t* data, uint64_t size);' "$E64H" && \
   grep -qF 'int elf64_map_blob(' "$E64H" && \
   grep -qF 'if (ehdr->e_type == ELF_ET_DYN)' "$E64C" && \
   grep -qF 'if (ehdr->e_type != ELF_ET_EXEC)' "$E64C" && \
   grep -qF 'cap_arch_map_page((uint64_t)(uintptr_t)pml4,' "$E64C"; then
    ok "O1. the ONE ELF mapper exists with §5.1's gates by name — magic-only dispatch, ET_DYN refused, nothing-but-ET_EXEC refused — and routes its segments through cap_arch_map_page's SAFE walk (a low-linked segment must shadow the shared kernel identity map, never write its leaf through a huge-page entry)"
else
    bad "O1. the ELF mapper's gates are gone, renamed, or routed back through user_map_page — an ET_DYN image mapped at its linked addresses with no bias, a non-ET_EXEC accepted by a mapper that believes its own header, or a low-linked segment (0x400000, inside the shared identity map) whose leaf lands in the kernel's own frames while the child's fetch still resolves through the supervisor huge page: the candidate #PFs at its first instruction while [LOADER] claims R-X"
fi
if grep -qF 'image_is_elf = elf_blob_has_magic(' "$CAPC" && \
   grep -qF 'if (!elf64_map_blob(' "$CAPC" && \
   grep -qF 'pd->user_rip   = image_is_elf ? elf_entry : image_vbase + m.image_entry;' "$CAPC"; then
    ok "O2. the create dispatches on the MAGIC: the one mapper owns every ELF's segments, the entry is e_entry, never the flat mapper's base+offset — image format is an image property, never a personality inference (§5.1)"
else
    bad "O2. the create's magic dispatch is gone — an ELF would enter at base+offset while its segments sit at their linked vaddrs (first fetch from bytes that are not code), or the dispatch became a personality decision §5.1 forbids"
fi
if grep -qF 'built_rsp = linux_initial_stack_build(' "$CAPC" && \
   grep -qF 'sp.region_vaddr = stack_base;' "$CAPC" && \
   grep -qF '(size_t)stk_pages * 4096 - 4096, &sp);' "$CAPC" && \
   grep -qF 'budget_stack_bytes < 2 * 4096u' "$CAPC"; then
    ok "O3. the §5.2 image is built into the child's contiguous stack with CHILD-vaddr pointers (region_vaddr), one page short of the top so the unconditional BIB write cannot clobber it, over an 8 KiB floor that names the BIB-page-plus-image minimum"
else
    bad "O3. the stack image's contract broke — pointers built from the host/phys region address (a libc dereferences argv[0] in its OWN space and faults on unmapped linear), the image spanning the BIB's top page (argc/auxv clobbered after they were built), or a floor that admits a stack too small to hold either"
fi
if grep -qF 'if (entropy_get(rnd, sizeof rnd) != ENTROPY_OK)' "$CAPC" && \
   grep -qF 'return CAP_ECONFLICT;' "$CAPC" && \
   grep -qF 'free_contiguous_frames_for_partition(linux_stack_phys,' "$CAPC"; then
    ok "O4. AT_RANDOM is entropy or refusal: an unseeded pool refuses the whole create (CAP_ECONFLICT), never a zero-filled substitute, and the refusal returns the half-built stack region to the partition"
else
    bad "O4. the entropy gate is gone — a stack image with zeroed AT_RANDOM bytes ships silently and every canary seeded from them is worth nothing, while the boot reads green"
fi
if grep -qF 'include_bytes!("../fixtures/e7_candidate.elf")' "$CAND" && \
   grep -qF -- '-Wl,-Ttext=0x400000' "$CBUILD" && \
   grep -qF -- '-no-pie' "$CBUILD" && \
   grep -qF 'crate::e7_candidate::spawn(' "$LRE" && \
   grep -qF 'crate::e7_candidate::CAND_NAME,' "$LRE"; then
    ok "O5. the census candidate is a committed ET_EXEC fixture (reproducible, non-PIE build line) and the sidecar spawns it beside the probe — the controlled pair the boot measurement reads"
else
    bad "O5. the candidate's spawn path is gone — the boot measures only the flat mapper again and §5.1/§5.2's real-image clauses stay argued rather than observed"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "linux_personality_check: all clauses hold"
    exit 0
fi
echo "linux_personality_check: $fails clause(s) failed"
exit 1

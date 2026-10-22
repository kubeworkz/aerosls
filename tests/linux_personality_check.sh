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
         user/sidecar/src/e7_probe.rs user/sidecar/fixtures/e7_linux_probe.S \
         user/sidecar/src/e7_firstuser.rs user/proto/src/manifest.rs \
         arch/x86/fpu_enable.h arch/x86/lazy_fpu.c arch/x86/idt.c \
         arch/x86/interrupt.asm arch/x86/switch_lazy.asm \
         kernel/kernel.c kernel/stubs.c tests/fpu_enable_host_test.c; do
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
LFU=user/sidecar/src/e7_firstuser.rs
MANR=user/proto/src/manifest.rs
CCT=tests/cap_create_sidecar_host_test.c
FPUH=arch/x86/fpu_enable.h
LFUC=arch/x86/lazy_fpu.c
IDTC=arch/x86/idt.c
ISRA=arch/x86/interrupt.asm
SWLA=arch/x86/switch_lazy.asm
KERC=kernel/kernel.c
STUB=kernel/stubs.c
FHUT=tests/fpu_enable_host_test.c
PHUT=tests/linux_personality_host_test.c

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
if grep -qF '218 => {' "$LRS" && \
   grep -qF 'sig.tid = args[0];' "$LRS" && \
   grep -qF '231 => Answer::Exit,' "$LRS" && \
   grep -qF '_ => Answer::Reply(0, E_NOSYS),' "$LRS"; then
    ok "K1. the answers are the census's core: set_tid_address echoes AND records the tid the seam's tkill matches against, exit_group reaps, the default arm still refuses ENOSYS with its number logged (the sig class's own answers are clause R)"
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
if grep -qF 'let expected = expected_req_len(num, &args);' "$LRS" && \
   grep -qF 'args[2] as usize > FD_PAYLOAD_CAP || rr.len != expected' "$LRS" && \
   grep -qF 'if rr.len != expected {' "$LRS" && \
   grep -qF 'const FD_PAYLOAD_CAP: usize = 1024;' "$LRS" && \
   grep -qF 'match console.write(&payload[..len]) {' "$LRS" && \
   grep -qF 'crate::linux::service(&RealKernel, &booted.console);' "$LRE"; then
    ok "M6. the sidecar checks EVERY request's attached bytes against expected_req_len (the kernel's attach twin: write's count, the sig class's act/set — a drifted attach arrives only as a wrong length and dies -EPROTO), the write chunk cap is intact, the perform happens on the loop's console, and service is handed that console"
else
    bad "M6. the payload invariant, the chunk cap, the console perform, or the service's console hand-off is gone — bytes the task did not write could be performed, or a write could never reach the serial log it exists to prove"
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
# O6/O7 hold §5.2's environment-crossing record (SIDECAR_TAG_ARGENV) at
# BOTH ends of the seam it crosses: the kernel's parse (bounded, refusing
# by name, consulted by the stack-image site with the v1 fallback intact)
# and the Rust format reference + the spawn sites that author the record.
# Every clause here is a way a Linux child silently gets the EMPTY
# environment again — the exact state this record exists to end — while
# every other §5.2 clause stays green reading argc alone. The cap host
# test's L3/L4 sections bite the bytes; the smoke's t49–t52 bite these.
if grep -qF '#define SIDECAR_TAG_ARGENV       0x000E' "$CAPH" && \
   grep -qF '#define SIDECAR_MANIFEST_MAX_ARGV  8' "$CAPH" && \
   grep -qF '#define SIDECAR_MANIFEST_MAX_ENVP  16' "$CAPH" && \
   grep -qF '#define SIDECAR_MANIFEST_ARGENV_BYTES 512' "$CAPH" && \
   grep -qF 'char     argenv_arena[SIDECAR_MANIFEST_ARGENV_BYTES];' "$CAPH" && \
   grep -qF 'case SIDECAR_TAG_ARGENV: {' "$CAPC" && \
   grep -qF 'if (ac > SIDECAR_MANIFEST_MAX_ARGV ||' "$CAPC" && \
   grep -qF 'if (q[j] == 0) return CAP_EINVAL;' "$CAPC" && \
   grep -qF 'if (q != rp + rlen) return CAP_EINVAL;' "$CAPC" && \
   grep -qF 'm.argc = ac;' "$CAPC" && \
   grep -qF 'for (uint32_t i = 0; i < m.argc; i++)' "$CAPC" && \
   grep -qF 'argv[m.argc] = 0;' "$CAPC" && \
   grep -qF 'for (uint32_t i = 0; i < m.envc; i++)' "$CAPC" && \
   grep -qF 'envp[m.envc] = 0;' "$CAPC" && \
   grep -qF 'argv[0] = pd->name;' "$CAPC"; then
    ok "O6. the ARGENV record exists by name with its bounds, the parse refuses by name (counts/arena ERANGE, interior NUL and trailing bytes EINVAL), and the §5.2 site consults it with the v1 fallback (argv = { name }, empty env) intact"
else
    bad "O6. the environment-crossing record is gone from one half of the seam — the parse would refuse every record as an unknown tag (flags=0, or ship a record whose strings can silently overflow/truncate), or the stack-image site would ignore it and hand every Linux child the empty environment again while every other §5.2 clause stays green"
fi
if grep -qF 'pub const TAG_ARGENV: u16 = 0x000E;' "$MANR" && \
   grep -qF 'pub const MAX_ARGV: usize = 8;' "$MANR" && \
   grep -qF 'pub const MAX_ENVP: usize = 16;' "$MANR" && \
   grep -qF 'pub const ARGENV_MAX_STR_BYTES: usize = 512;' "$MANR" && \
   grep -qF 'recs.push((TAG_ARGENV, p));' "$MANR" && \
   grep -qF 'TAG_ARGENV => {' "$MANR" && \
   grep -qF 'pub const LINUX_DEFAULT_ENV: &[&str] = &["PATH=/bin", "HOME=/root"];' "$LRS" && \
   grep -qF 'argenv: Some(ArgEnv::new(&[PROBE_NAME], LINUX_DEFAULT_ENV)),' "$LPRO" && \
   grep -qF 'argenv: Some(ArgEnv::new(&[CAND_NAME], LINUX_DEFAULT_ENV)),' "$CAND" && \
   grep -qF 'argenv: Some(ArgEnv::new(&[USER_NAME], LINUX_DEFAULT_ENV)),' "$LFU"; then
    ok "O7. the Rust format reference carries the record with the kernel's bounds and emits it, the environment crosses from one shared default list, and all three Linux spawn sites author the record (firstuser, probe and candidate — the pair stays a pair)"
else
    bad "O7. the record's Rust half or a spawn site is gone — the format reference would refuse its own record as unknown, the two halves of the wire format would drift apart silently, or a Linux child would cross with no record at all and be handed the empty environment while its sibling gets a real one"
fi
# O8 holds the mapper against the assumption that killed the first user
# (§10.13's step-0 diagnosis, MEASURED): elf64_map_blob copied each LOAD
# from segment offset 0 into a frame mapped at p_vaddr, so a segment
# linking MID-PAGE landed every byte (p_vaddr & 0xFFF) early in vaddr
# space — the firstuser's .init_array read back 0x28 instead of 0x800ae00,
# __libc_start_init's `call *(%rbx)` jumped there, and the child died at
# rip=0x28 before main while [LOADER] claimed the load succeeded. Every
# committed image before it had page-aligned LOADs, which is why the
# census could not see it. The cap host test's E8a section EXECUTES the
# case (and the shared-page refusal) — the smoke's t53/t54 bite both.
if grep -qF 'uint64_t page_base = s->p_vaddr & ~(uint64_t)4095u;' "$E64C" && \
   grep -qF 'n_pages = (lead + s->p_memsz + 4095) / 4096;' "$E64C" && \
   grep -qF 'e6_memcpy((uint8_t*)frame + frame_off,' "$E64C" && \
   grep -qF 'if (a_lo < b_hi && b_lo < a_hi) {' "$E64C" && \
   grep -qF 'share a page — one frame per page' "$E64C" && \
   grep -qF 'E8a: the NON-ALIGNED segment' "$CCT" && \
   grep -qF 'E8a: two LOADs sharing a page are REFUSED' "$CCT"; then
    ok "O8. a PT_LOAD that starts MID-PAGE lands at its LINKED vaddr: the page base is the mapping anchor and the segment's lead is where its bytes start inside the frame (both zero for an aligned segment, so the images that already worked are unmoved), two LOADs sharing a page are refused by name before the first frame, and tests/cap_create_sidecar_host_test.c's E8a section executes the case — the measured killer of the first user"
else
    bad "O8. the mapper's page-alignment handling is gone — a segment linking mid-page lands (p_vaddr & 0xFFF) bytes early again, every global in it reads the file that much further on, and a libc-linked binary dies at rip=0x28 before main while [LOADER] reports a successful load; the shared-page refusal going with it would let a second segment silently discard the first's bytes and permissions through the one frame per page"
fi

# ─── Q. The SSE gate (§10.13's step-1 death, increment 9): CR4.OSFXSR and its order ──
# The designated first user reached `main` and then died at musl abort()'s
# `pxor %xmm0,%xmm0` (rip=0x80525b0, error=0x0 — #UD, which carries no error
# code) because boot.asm wrote CR4.PAE and nothing else: with OSFXSR clear,
# EVERY SSE instruction a ring-3 task executes is #UD, which is every real
# libc and every Rust binary. The gate is fpu_enable_sse()
# (arch/x86/lazy_fpu.c), armed on the BSP in kernel.c's 4b block before
# syscall_gate_init() opens ring 3 at all. Every clause below is a way it is
# silently a no-op again while the boot log still reads green: a call that
# never happens, a bit at the wrong position, an ldmxcsr emitted BEFORE the
# CR4 write (ldmxcsr is itself an SSE instruction — #UD with OSFXSR clear,
# the exact death this gate exists to end), a vector-7 chain with a gap in
# it (an #NM with no handler is the same silent-triple-fault class), or a
# CR0.TS armed somewhere the dormant lazy-FPU path cannot service. The host
# test (tests/fpu_enable_host_test.c) pins the bit arithmetic and the
# EMITTED ORDER of the asm; these clauses hold the wiring around it. The
# smoke's t70–t75 bite these.
lf_cpuid=$(grep -nF '__asm__ volatile("cpuid"' "$LFUC" | head -1 | cut -d: -f1)
lf_refuse=$(grep -nF 'if (!fpu_cpuid_has_sse(edx)) {' "$LFUC" | head -1 | cut -d: -f1)
lf_cr4=$(grep -nF 'cr4 = fpu_cr4_enable_sse(cr4);' "$LFUC" | head -1 | cut -d: -f1)
lf_cr0=$(grep -nF 'cr0 = fpu_cr0_enable_fpu(cr0);' "$LFUC" | head -1 | cut -d: -f1)
lf_fninit=$(grep -nF '__asm__ volatile("fninit")' "$LFUC" | head -1 | cut -d: -f1)
lf_ldmx=$(grep -nF '__asm__ volatile("ldmxcsr' "$LFUC" | head -1 | cut -d: -f1)
kc_call=$(grep -nF 'fpu_enable_sse();' "$KERC" | head -1 | cut -d: -f1)
kc_gate=$(grep -nF 'syscall_gate_init();' "$KERC" | head -1 | cut -d: -f1)
if grep -qF '#define FPU_CR4_OSFXSR' "$FPUH" && \
   grep -qF '(1ull << 9)' "$FPUH" && \
   grep -qF '#define FPU_CR4_OSXMMEXCPT' "$FPUH" && \
   grep -qF '(1ull << 10)' "$FPUH" && \
   grep -qF '#define FPU_CR0_MP' "$FPUH" && \
   grep -qF '#define FPU_SSE_MXCSR_DEFAULT 0x1F80u' "$FPUH" && \
   grep -qF 'void fpu_enable_sse(void)' "$FPUH" && \
   grep -qF 'void fpu_enable_sse(void) {' "$LFUC" && \
   grep -qF 'fpu_cr4_enable_sse(' "$LFUC" && \
   grep -qF 'fpu_cr0_enable_fpu(' "$LFUC" && \
   grep -qF 'fpu_cpuid_has_sse(' "$LFUC" && \
   grep -qF '#include "../arch/x86/fpu_enable.h"' "$KERC" && \
   [ -n "$kc_call" ] && [ -n "$kc_gate" ] && [ "$kc_call" -lt "$kc_gate" ]; then
    ok "Q1. the SSE gate exists with its SDM bits by name — CR4.OSFXSR (bit 9), OSXMMEXCPT (bit 10), CR0.MP, the 0x1F80 MXCSR reset — fpu_enable_sse() consumes the pure arithmetic rather than re-deriving literals the host test cannot see, and kernel.c CALLS it before syscall_gate_init() opens ring 3 at all"
else
    bad "Q1. the SSE gate's bits, its body, or its BSP call site are gone/renamed — every ring-3 SSE instruction is #UD again (musl abort()'s pxor, measured rip=0x80525b0), the enable writes literals no host test pins, or the gate is compiled and never runs on anybody"
fi
if [ -n "$lf_cpuid" ] && [ -n "$lf_refuse" ] && [ -n "$lf_cr4" ] && \
   [ -n "$lf_cr0" ] && [ -n "$lf_fninit" ] && [ -n "$lf_ldmx" ] && \
   [ "$lf_cpuid" -lt "$lf_refuse" ] && [ "$lf_refuse" -lt "$lf_cr4" ] && \
   [ "$lf_cr4" -lt "$lf_cr0" ] && [ "$lf_cr0" -lt "$lf_fninit" ] && \
   [ "$lf_fninit" -lt "$lf_ldmx" ] && \
   grep -qF 'fpu_enable_host_test.c' "$LFUC"; then
    ok "Q2. the enable's order is the safe one: CPUID first (a CPU without SSE gets a refusal line, not a #GP/triple fault), then CR4.OSFXSR, then CR0's EM/TS clear, then fninit, then ldmxcsr — ldmxcsr is itself an SSE instruction and #UDs before the CR4 write, which is the death this gate exists to end"
else
    bad "Q2. the SSE enable's order broke — ldmxcsr or fninit emitted before the CR4/CR0 writes (#UD on the gate's own first instruction, the same silent death class), the CPUID refusal bypassed (a #GP on the CR4 write of a CPU without SSE), or the host test that pins the emitted order is no longer referenced"
fi
if grep -qF 'set_idt_gate( 7, (uint64_t)isr7_stub,' "$IDTC" && \
   grep -qF 'isr7_stub' "$ISRA" && \
   grep -qF 'handle_device_not_available' "$STUB" && \
   grep -qF '__asm__ volatile("clts")' "$LFUC"; then
    ok "Q3. the vector-7 chain is intact end to end — idt.c's gate, isr7_stub, the stubs' route to handle_device_not_available_fault, and its clts that clears CR0.TS before touching state — so arming TS later cannot produce an unhandled #NM (the same silent-triple-fault class as the #UD this increment removed)"
else
    bad "Q3. a link in the #NM chain is gone — a vector-7 trap with no gate or no handler is a double/triple fault under -no-reboot (no trace, no panic), and a handler that lost its clts would #NM-loop on its own first SSE instruction"
fi
lazy_calls=$(grep -rn 'perform_lazy_context_switch(' kernel arch --include=*.c 2>/dev/null \
              | grep -vE ':[[:space:]]*(\*|//)' | wc -l)
if [ "$lazy_calls" -eq 0 ] && \
   grep -qF 'referenced by nothing' "$LFUC" && \
   grep -qF 'no caller' "$FPUH"; then
    ok "Q4. the lazy-FPU path stays DORMANT and says so — no .c calls perform_lazy_context_switch(), so CR0.TS is never armed, #NM never fires, and the xsave/xrstor handler (which would itself need CR4.OSXSAVE) is unreachable; XMM state is therefore NOT switched per task, and both files name that gap instead of papering over it"
else
    bad "Q4. a caller of the lazy switch appeared, or the dormancy claim is gone — TS would arm on a switch-in while the handler's xsave needs CR4.OSXSAVE (which is deliberately never set): the first #NM would #UD inside the handler, and the doc's 'XMM is not switched' claim would silently stop being true"
fi

# ─── R. The signal seam (§10.13's step 2): rt_sigaction 13, rt_sigprocmask 14, tkill 200 ──
# The firstuser died at abort()'s hlt because all three numbers refused:
# raise(SIGABRT) never terminated the task, so abort walked into its guard
# instruction (rip=0x805260c, the wall the SSE gate left behind). The seam
# is a three-part split: the KERNEL copies the bytes (act/set out of the
# child, every refusal decided at the FIRST trap before any channel), the
# SIDECAR owns every semantic decision (disposition, mask, pending), and
# a default-terminate delivery's faithful shape is death INSIDE the
# delivering syscall — no reply, reaped through exit_group's path, the log
# naming WHICH decision killed the task. Every clause below is a way the
# boot gets its [FAULT] line back — or a lie in a place the census reads
# green while raise() silently succeeds.
if grep -qF 'struct LinuxSigCopyPlan {' "$LABH" && \
   grep -qF 'p.refuse = -LINUX_EINVAL;' "$LABC" && \
   grep -qF 'p.refuse = -LINUX_EFAULT;' "$LABC" && \
   ! grep -qF 'p.refuse = -LINUX_ENOSYS;' "$LABC"; then
    ok "R1. linux_sig_copy_plan is the crossing's mechanism half with BOTH of its first-trap refusals as negative errnos — a foreign sigsetsize EINVAL, a non-canonical act/set span EFAULT — and the out-pointer ENOSYS it once carried is GONE (increment 12 planned that span instead: clause set T)"
else
    bad "R1. the plan or one of its refusals changed shape — a foreign sigsetsize would have its bytes silently reinterpreted, or the retired out-pointer ENOSYS is back (a span now owned by linux_fd_reply_plan would be double-trapped, and the reply payload would never be staged)"
fi
if grep -qF 'linux_sig_copy_plan(num, largs);' "$DISP" && \
   grep -qF 'return (uint64_t)(int64_t)splan.refuse;' "$DISP" && \
   grep -qF 'plan.src = splan.src;' "$DISP" && \
   grep -qF 'fwd_len += plan.len;' "$DISP"; then
    ok "R2. the dispatch runs the plan BEFORE any channel post (the refusal returns from the first trap) and merges its span into the forward — the act/set bytes ride the message the sidecar's 13/14 arms read"
else
    bad "R2. the dispatch no longer consumes the plan in that order — a refusal would post first (the exact shape R1 exists to prevent), or the sidecar's sig arms would read a payload the kernel never attached and answer a refusal to a request it never saw"
fi
if grep -qF '{  13, LSC_SIG,' "$LABC" && \
   grep -qF '{  14, LSC_SIG,' "$LABC"; then
    ok "R3. both sig numbers are classed LSC_SIG — recorded at the sidecar, refused only by name, so the class itself declares what this seam may answer"
else
    bad "R3. a sig number fell out of the class table — the dispatch's plan branch never runs for it, so its payload crosses unmeasured (or not at all) while the census still shows the row green"
fi
if grep -qF 'LINUX_RT_SIGACTION => {' "$LRS" && \
   grep -qF 'LINUX_RT_SIGPROCMASK => {' "$LRS" && \
   grep -qF 'LINUX_TKILL => {' "$LRS" && \
   grep -qF 'ACT_TERMINATE => Answer::SignalKill(args[1]),' "$LRS" && \
   grep -qF '1 => Answer::SignalKill(kill_sig),' "$LRS" && \
   grep -qF 'Answer::SignalKill(kill_sig) => {' "$LRS"; then
    ok "R4. the sidecar answers all three numbers, and every path that RUNS a default-terminate action (tkill's own delivery and the unblock arm's reveal) converges on SignalKill — the one variant service reaps through exit_group's path with no reply, exactly where real Linux dies inside the delivering syscall"
else
    bad "R4. an arm or the SignalKill convergence is gone — raise(SIGABRT) would refuse or reply successfully, abort() would walk on into its hlt, and the boot would show [FAULT] at 0x805260c again (§10.13's step-1 wall)"
fi
if grep -qF 'E_NOSYS), /* handler install not landed' "$LRS" && \
   grep -qF 'if sig.tid == 0 || args[0] as u32 != sig.tid as u32 {' "$LRS" && \
   grep -qF 'sig.pending |= 1u64 << (args[1] - 1);' "$LRS"; then
    ok "R5. the seam's honesty lines hold: a handler install refuses ENOSYS by name BEFORE any mutation (v1 has no signal frames to deliver one on), tkill's tid is the set_tid_address echo at u32 width (ESRCH otherwise — never silently re-targeted), and a blocked signal PENDS so the unblock arm is where it delivers"
else
    bad "R5. one of the seam's honesty lines is gone — a handler install would record a disposition this shim can never invoke (every later delivery a coin flip), a tid mismatch could kill a task that never asked, or a blocked raise() would be lost outright instead of pending"
fi
if grep -qF 'linux_sig_copy_plan(' "$PHUT" && \
   grep -qF 'p.refuse == -LINUX_EINVAL && p.len == 0' "$PHUT"; then
    ok "R6. the plan is host-tested against the REAL kernel/linux_abi.c — both refusals by name, the EFAULT bounds, both lengths, the precedence and the total number space (run_all compiles and runs it)"
else
    bad "R6. the host test no longer drives the plan — the seam's pure half would be argued from the guard's greps alone, every branch reachable only at boot"
fi
if grep -qF 'fmt_sig_line(&mut ex, b" ok sig=", args[1], b"\n")' "$LRS" && \
   grep -qF 'fmt_sig_line(&mut ex, b" sig=", kill_sig,' "$LRS" && \
   grep -qF 'SignalKill(u64),' "$LRS"; then
    ok "R7. the signal NUMBER rides its line: tkill's ok prints the sig it was asked for (the pending-then-unblock flow's evidence), the kill line prints the signal it is delivering (carried on the Answer itself) — WHICH signal is read off the log, never inferred from which arms ran"
else
    bad "R7. a signal number no longer rides its line — tkill's pending ask or the killing delivery goes back to arithmetic (the exact inference the child= field removed for refusals: a count that fits three children is not a line that proves one)"
fi

if grep -qF 'fmt_sig_line(&mut ex, b" write ok len=",' "$LRS" && \
   grep -qF 'for &b in payload[..len].iter().take(24)' "$LRS" && \
   grep -qF 'push_bytes(&mut ex, 0, b" -> ENOSYS mmap(len="' "$LRS" && \
   grep -qF 'off = dec_into(&mut ex, off, args[1]);' "$LRS"; then
    ok "R8. the SIGABRT reading's instrument holds: the workload's own console bytes ride its write lines (len + a 24-byte excerpt, child-attributed) and mmap refusals carry their request (len/prot/flags) — 'alloc failed after ENOSYS' is read off the E7 stream, never inferred from a merged console drain whose order is the flush, not the writer"
else
    bad "R8. the attribution is gone — the alloc-failure text would exist only in the merged console drain (any child of the environment could have written it) and the mmap refusal goes back to a bare ENOSYS with no request to name (the proven chain becomes an inference again)"
fi

# ─── S. poll (7) — the run's LAST ENOSYS answer (§10.13's step-2 coda) ────
# The firstuser's num=00000007 line is POLL, not munmap — §10.13's log
# annotation mislabelled it from the census row's COUNT (`11 munmap 7`):
# munmap is 11 and never fires before the kill, while Rust std's
# lang_start_internal polls fds {0,1,2} with a ZERO timeout before it
# installs any signal handler, and an errno outside its tolerated set
# (EAGAIN/ENOMEM/EINVAL, its own bitmask) aborts the task — which is why
# the run died at raise(SIGABRT) four traps in. The answer is
# deliberately small: a zero-timeout poll reports 0 events (this shim
# has no console-input readiness state — under-reporting readiness can
# only send a caller back to its own loop, never into an action built
# on readiness nobody claimed), a poll that would WAIT is refused by
# name at BOTH ends (it needs a wake against state that does not exist),
# and no bytes cross either way (a count of 0 promises no revents —
# the copy-IN that would carry them stays increment 8's named gap).
# Every clause below is a way that answer silently becomes a lie while
# the rest of this guard stays green: the predicate rewritten until it
# refuses every poll (the wall back by name) or grants a wait whose
# wake cannot come, the first-trap refusal deleted (the waiting poll
# posts and parks), the sidecar arm gutted, or the pure half untested.
if grep -qF '#define LINUX_POLL  7u' "$LABH" && \
   grep -qF 'int linux_poll_refuses_blocking(uint64_t num, uint64_t timeout);' "$LABH" && \
   grep -qF 'int linux_poll_refuses_blocking(uint64_t num, uint64_t timeout)' "$LABC" && \
   grep -qF 'return num == LINUX_POLL && (int32_t)timeout != 0;' "$LABC"; then
    ok "S. poll's waiting refusal is the named pure contract — the census's own number, Linux's INT timeout (the low 32 bits, sign-extended), refused iff the call would WAIT, declared where the dispatcher and the host test both read it"
else
    bad "S. the poll contract is gone or rewritten — the dispatcher has no predicate to consult (a waiting poll parks a task whose wake cannot come) and a rewritten one can only be argued from prose while the host test bites a function nothing calls"
fi
poll_ref=$(grep -nF 'linux_poll_refuses_blocking(num, largs[2]))' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$poll_ref" ] && \
   sed -n "$((poll_ref + 1)),$((poll_ref + 6))p" "$DISP" | grep -qF 'console-input state' && \
   sed -n "$((poll_ref + 1)),$((poll_ref + 6))p" "$DISP" | grep -qF -- '(uint64_t)(int64_t)(-LINUX_ENOSYS);'; then
    ok "S1. the FIRST TRAP refuses a waiting poll before any post — a request property (a timeout word), so no channel is needed to say it (mprotect's posture), logged by name with its trigger, returning -ENOSYS in the task's own units"
else
    bad "S1. the dispatcher's poll refusal is gone or no longer returns from the first trap — a waiting poll would post and park on a channel whose wake cannot come (no console-input state), and the sidecar twin would then be the only wall left"
fi
poll_arm=$(grep -nF 'if (args[2] as i32) == 0 {' "$LRS" | head -1 | cut -d: -f1)
if grep -qF '        7 => {' "$LRS" && \
   [ -n "$poll_arm" ] && \
   sed -n "$((poll_arm + 1))p" "$LRS" | grep -qF 'Answer::Reply(0, STATUS_OK)' && \
   sed -n "$((poll_arm + 3))p" "$LRS" | grep -qF 'Answer::Reply(0, E_NOSYS)'; then
    ok "S2. the sidecar's 7 arm answers the ZERO-timeout probe 0 events and the waiting poll ENOSYS by name — its twin of the first-trap refusal (N3's both-ends posture), the zero timeout decided at the arm's own literal"
else
    bad "S2. the poll arm is gone or either answer changed — a granted wait (a wake that can never come, or a five-second poll answered 0 into the caller's own loop) or an ENOSYS for the probe (the firstuser's measured wall, back by name)"
fi
if grep -qF 'LINUX_POLL == 7' "$PHUT" && \
   grep -qF 'linux_poll_refuses_blocking(LINUX_POLL, 0)' "$PHUT" && \
   grep -qF '0x100000000ULL' "$PHUT"; then
    ok "S3. the predicate is host-tested against the REAL kernel/linux_abi.c — the zero-timeout probe, the timed and infinite refusals, the int-width rule (0x1_00000000 is a zero timeout) and the total number space (run_all compiles and runs it)"
else
    bad "S3. the host test no longer drives the predicate — the run's last-ENOSYS fix would be argued from the guard's greps alone, every branch reachable only at boot"
fi

# ─── T: the reply payload's copy-IN (E7 increment 12, §10.13's step 3) ────
# §10.10's named gap lands: rt_sigaction/rt_sigprocmask's oldact/oldset
# write back into the child, and the run's remaining first-trap
# refusals (13/14) retire. The seam is three pieces and a lifecycle:
# the first-trap destination plan, the two-phase writable copy, the
# deliver's three shapes, and the stage cleared at four places. Every
# clause below is a way this becomes the silent lie it was built to
# prevent — bytes into a destination no trap validated, a write through
# a read-only page, a stale stage aiming a payload at a reused slot's
# successor, or a success reply whose promised payload never crossed.
if grep -qF 'struct LinuxFdReplyPlan {' "$LABH" && \
   grep -qF 'struct LinuxFdReplyPlan linux_fd_reply_plan(uint64_t num,' "$LABH" && \
   grep -qF 'int linux_user_copy_in(const uint8_t *src, uint64_t root,' "$LABH" && \
   grep -qF 'int linux_reply_shape_ok(uint64_t num, uint64_t ret, uint32_t n);' "$LABH" && \
   grep -qF 'struct LinuxFdReplyPlan linux_fd_reply_plan(uint64_t num, const uint64_t args[6])' "$LABC" && \
   grep -qF 'int linux_user_copy_in(const uint8_t *src, uint64_t root,' "$LABC" && \
   grep -qF 'int linux_reply_shape_ok(uint64_t num, uint64_t ret, uint32_t n)' "$LABC"; then
    ok "T. the copy-IN is a named pure contract, declared where both ends read it: the destination plan, the two-phase copy, and the ret-shape predicate — each with a host test instead of a boot"
else
    bad "T. one of the three copy-IN functions is gone or un-declared — the reply payload would have no first-trap validator (bytes aimed wherever the reply says), no writable-boundary copy, or no rule keeping header, payload and rax one truth"
fi
disp_plan=$(grep -nF 'linux_fd_reply_plan(num, largs);' "$DISP" | head -1 | cut -d: -f1)
disp_clear=$(grep -nF 'cur->reply_stage_valid = 0;' "$DISP" | head -1 | cut -d: -f1)
disp_set=$(grep -nF 'cur->reply_stage_valid = 1;' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$disp_plan" ] && [ -n "$disp_clear" ] && [ -n "$disp_set" ] && \
   [ "$disp_clear" -lt "$disp_plan" ] && \
   [ "$disp_plan" -lt "$disp_set" ] && \
   sed -n "$((disp_plan + 1)),$((disp_plan + 10))p" "$DISP" | grep -qF '(reply plan)' && \
   sed -n "$((disp_plan + 1)),$((disp_plan + 10))p" "$DISP" | grep -qF '(uint64_t)(int64_t)rp.refuse;'; then
    ok "T1. the FIRST TRAP plans the destination (clear → plan → stage) — a span ring 3 could not write refuses -EFAULT before any post, and the stage is set only after every other first-trap refusal has had its say, so no refusal leaves a destination behind"
else
    bad "T1. the dispatch no longer plans/stages in that order — a bad destination would post first (the exact shape T exists to prevent), or a refusal would leave a stale stage aiming the next reply's payload at a span nothing validated"
fi
if grep -qF 'reply_stage_valid' "$PDH" && \
   grep -qF 'pd->reply_stage_valid = 0;' "$PDC" && \
   grep -qF 'pd->reply_stage_num == expect_num' "$CANC" && \
   grep -qF 'pd->reply_stage_valid = 0;' "$CANC" && \
   grep -qF 'reply_stage_len' "$CANC"; then
    ok "T2. the stage's four-place lifecycle holds — set at the first trap, consumed-and-cleared at the deliver (only when the stage's num is THIS request's), cleared at slot reuse, declared on the descriptor: a stale destination cannot aim a payload at a reused slot's successor"
else
    bad "T2. a stage clear or the deliver's consume is gone — an unanswered copy-in destination survives into a slot whose pid it never belonged to, and the next reply payload lands in the successor's address space"
fi
deliver_buf=$(grep -nF '+ LINUX_FD_WRITE_CHUNK];' "$CANC" | head -1 | cut -d: -f1)
if [ -n "$deliver_buf" ] && \
   grep -qF 'out.len < sizeof(struct SLSLinuxSyscallRequest)' "$CANC" && \
   grep -qF '!staged || n > slen' "$CANC" && \
   grep -qF 'linux_reply_shape_ok(expect_num, hdr_ret, n)' "$CANC" && \
   grep -qF 'linux_user_copy_in(rbuf +' "$CANC" && \
   grep -qF 'uint64_t rax = linux_syscall_apply_reply(' "$CANC"; then
    ok "T3. the deliver accepts exactly three shapes — 72 (a SUCCESS with an owed payload refused -EPROTO), 72+n within the staged clamp with a matching echo/status/ret, and CAP_ERR_PROTO otherwise — apply first, then the two-phase copy, then rax"
else
    bad "T3. a shape arm is gone — a payload would be copied for a number that staged none, beyond the clamp, past a refusal, or with a ret its number contradicts (the one-truth rule with bytes attached)"
fi
if grep -qF 'int require_rw)' "$LABC" && \
   grep -qF 'if (require_rw && !(e & LAB_PTE_RW)) return -LINUX_EFAULT;' "$LABC" && \
   grep -qF '#define LAB_PTE_RW' "$LABC" && \
   grep -qF 'Phase 1 — validate the WHOLE span' "$LABC" && \
   grep -qF 'Phase 2 — write.' "$LABC"; then
    ok "T4. the copy-IN validates (present, USER, WRITABLE at every level) the WHOLE span before writing a byte — the sidecar can never launder a write through a page the child could not have written, and a refusal mutates nothing"
else
    bad "T4. the writable check or the two-phase order is gone — a read-only page under a payload becomes writable-by-sidecar, or a span half-writes before its second page refuses"
fi
if grep -qF 'ReplyBytes(u64, usize, [u8; SIGACT_BYTES]),' "$LRS" && \
   grep -qF 'if args[2] == 0 || len != want || len > bytes.len() {' "$LRS" && \
   grep -qF 'reply payload mismatch -> -EPROTO' "$LRS" && \
   grep -qF 'reply_bytes(k, ch_w, rr.tag, num, ret, &bytes[..len]);' "$LRS" && \
   grep -qF 'old_act_bytes(sig, args[0])' "$LRS"; then
    ok "T5. the sidecar's twin bound — a ReplyBytes answer must equal the forwarded out-pointer's clamp or it goes out as num=0 → -EPROTO, the old disposition is captured BEFORE any mutation, and only service() sends the bytes (neither end load-bearing alone)"
else
    bad "T5. the sidecar's bound or the payload arm is gone — bytes could cross without a destination the first trap validated, or oldact could carry the NEW disposition (the caller reads a future that already moved)"
fi
if grep -qF 'linux_fd_reply_plan(LINUX_RT_SIGACTION, a)' "$PHUT" && \
   grep -qF 'linux_user_copy_in(src, root' "$PHUT" && \
   grep -qF 'linux_reply_shape_ok(LINUX_READ, 32, 32)' "$PHUT" && \
   grep -qF 'c_page0[0x100] == 0x77' "$PHUT"; then
    ok "T6. the host test drives all three halves against the REAL linux_abi.c — the plan's branches, the two-phase copy's bytes-unchanged refusals, and the ret-shape rule (run_all compiles and runs it)"
else
    bad "T6. the host test no longer drives the copy-IN — every boundary would be argued from these greps alone, reachable only at boot"
fi

# ─── U. The round trip preserves the caller-saved registers ─────────────────
# The wall the firstuser's run died on once step 3 cleared the SIGPIPE
# assert: musl's __libc_sigaction holds `act` in r8 across its unmask
# rt_sigprocmask, the entry path destroys r8 by contract, and the resume
# returned kernel scratch — Ring-3 #PF at rip=0x8054bdf (`mov 0x88(%r8),
# %eax`), addr = r8+0x88 = 0x90/0x91 (two boots, two garbage r8s), exit
# code 139. Linux changes only rax (result), rcx and r11 across a
# syscall, so the SIX argument registers must survive the whole round
# trip: captured at entry (clause C), snapshotted into park_ctx when the
# task parks, re-stamped into the per-CPU block by BOTH resume paths,
# and restored at syscall_return_path — four places, each one missing
# store handing ring-3 kernel scratch on every syscall.
restore_desc=""
restore_ok=1
for pair in "rdi 50" "rsi 58" "rdx 60" "r10 68" "r8 70" "r9 78"; do
    set -- $pair
    if ! grep -qE "mov +$1, +\\[gs:0x$2\\]" "$ASM"; then
        restore_ok=0
        restore_desc="$restore_desc [$1]=gs:0x$2"
    fi
done
if [ "$restore_ok" -eq 1 ]; then
    ok "U. syscall_return_path restores all six argument registers from this entry's capture ([gs:0x50..0x80)) — Linux's only-changed-registers rule, so a live value across a syscall survives to ring-3"
else
    bad "U. syscall_return_path is missing restore(s):$restore_desc — every syscall hands ring-3 kernel scratch in those registers (the 0x8054bdf wall, by name)"
fi

# The restore must not carry the return value away with it: RAX is the
# syscall's answer, and a restore written through RAX would hand ring-3
# the capture word instead of the result — the one register the round
# trip must NOT put back.
return_block="$(sed -n '/^syscall_return_path:/,/swapgs/p' "$ASM")"
if printf '%s\n' "$return_block" | grep -qw 'rax'; then
    bad "U1. the return path touches RAX between the pops and the swapgs — ring-3 would read a capture word as the syscall's return value"
else
    ok "U1. the return path never touches RAX — the syscall's result survives the restore"
fi

capfn="$(awk '/^static void proc_capture_entry_regs/{f=1} f{print} f&&/^}/{exit}' "$PDC")"
if printf '%s\n' "$capfn" | grep -qF '%%gs:0x50' && \
   printf '%s\n' "$capfn" | grep -qF 'ctx->linux_args[5]'; then
    ok "U2. proc_capture_entry_regs snapshots the six argument registers into park_ctx beside the pushed set — a parked task's registers outlive the per-CPU block every later entry overwrites"
else
    bad "U2. the entry capture no longer reads [gs:0x50..0x80) into park_ctx — a parked task loses its argument registers to the next trap on that core, and every resume after it restores someone else's"
fi

u_loads="$(grep -cF 'movq 72(%1), %%rax' "$PDC")"
u_stores="$(grep -cF 'movq %%rax, %%gs:0x50' "$PDC")"
if [ "$u_loads" -ge 2 ] && [ "$u_stores" -ge 2 ]; then
    ok "U3. BOTH resume paths re-stamp [gs:0x50..0x80) from park_ctx (load and store, two each) before the re-run and before the sysret — a resume returns the parked task's own registers, never the last entry's"
else
    bad "U3. only $u_loads load(s) / $u_stores store(s) of the linux_args re-stamp (need 2 each) — one resume path now hands its task whatever registers the previous syscall entry left on that core"
fi

if grep -qE 'uint64_t +linux_args\[6\];' "$PDH"; then
    ok "U4. CapParkCtx carries linux_args[6] after user_rsp — the 72..112 byte window both resumes read and the host test pins"
else
    bad "U4. CapParkCtx has no linux_args[6] — the capture has nowhere to live and the resume's 72(%1) reads would walk off the struct"
fi

if grep -qF 'offsetof(struct CapParkCtx, linux_args) == 72' "$CCT" && \
   grep -qF 'sizeof(struct CapParkCtx) == 120' "$CCT"; then
    ok "U5. the host test pins the layout the resume asm indexes by byte — linux_args at 72 (p[9]), the full ctx 120 — so a reordered field is a red host test, not a boot that sysrets misaligned registers"
else
    bad "U5. the cap_create host test no longer pins CapParkCtx's layout — the resume asm's 72(%1)…112(%1) offsets would be argued from these greps alone"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "linux_personality_check: all clauses hold"
    exit 0
fi
echo "linux_personality_check: $fails clause(s) failed"
exit 1

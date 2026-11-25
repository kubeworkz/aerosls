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
         kernel/kernel.c kernel/stubs.c tests/fpu_enable_host_test.c \
         tests/user_map_identity_host_test.c tests/linux_clone_host_test.c; do
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
# P (increment 26, the signal frame) reads these: the frame's host test, and
# the table test that carries the "15 is not in the table yet" absence.
SIGFRAME_HUT=tests/linux_sigframe_host_test.c
SYSTAB=tests/linux_syscall_table_host_test.c
# P5–P8 (increment 27, the delivery) read these: the delivery's host test, the
# sidecar source (its park-interrupt rule and E_INTR code) and the CAP map.
SIGDELIVER_HUT=tests/linux_sigdeliver_host_test.c
LINUXRS=user/sidecar/src/linux.rs
CAPH=kernel/cap.h
FHUT=tests/fpu_enable_host_test.c
PHUT=tests/linux_personality_host_test.c
HUMAP=tests/user_map_identity_host_test.c
LTHH=kernel/linux_thread.h
LTHC=kernel/linux_thread.c

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
if grep -qF 'crate::linux::service(&RealKernel, &booted.console, &mut booted.proc.vfs);' "$LRE" && \
   grep -qF 'crate::linux::append_wait(' "$LRE"; then
    ok "K. the entry loop services the Linux children (with the VFS the file class now needs) AND adds their channels to the quiesce wait set (a forward wakes the sidecar, not an idle pass)"
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
   grep -qF 'if (num == LINUX_WRITE && plan.len) largs[2] = plan.len;' "$DISP" && \
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
   grep -qF 't.fds = [FdSlot { used: false, kind: 0, vfs_fd: 0, flags: 0,' "$LRS" && \
   grep -qF 'members: 0, sum: 0xFFFF_FFFF, sum_len: 0 };' "$LRS" && \
   grep -qF 't.fds.iter_mut().take(3)' "$LRS"; then
    ok "M5. the sidecar's fd table exists per task (console slots plus step 4's File kind carrying the VFS fd and flags), is born with console stdio (0/1/2), and an unknown descriptor is EBADF — CAP_EBADF pinned at -2 so §5.7 delivers 9"
else
    bad "M5. the fd table or its EBADF arm is gone — a descriptor nobody registered would be performed (or fake-answered) instead of refused, and a drifted E_BADF would surface a different errno than the policy meant"
fi
if grep -qF 'let (lo, hi) = expected_req_range(num, &args);' "$LRS" && \
   grep -qF 'args[2] as usize > FD_PAYLOAD_CAP || rr.len != lo' "$LRS" && \
   grep -qF 'if rr.len < lo || rr.len > hi {' "$LRS" && \
   grep -qF 'const FD_PAYLOAD_CAP: usize = 1024;' "$LRS" && \
   grep -qF 'match console.write(&payload[..len]) {' "$LRS" && \
   grep -qF 'crate::linux::service(&RealKernel, &booted.console, &mut booted.proc.vfs);' "$LRE"; then
    ok "M6. the sidecar checks EVERY request's attached bytes against expected_req_range — exact for write/sig (the kernel's attach twin), a cap-bounded range for the path numbers (rename carries two strings) — a drifted attach arrives only as a wrong length and dies -EPROTO, the write chunk cap is intact, the perform happens on the loop's console, and service is handed that console (plus the VFS)"
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
if grep -q 'g->brk_cur = 0;' "$LTHC" && \
   grep -q 'pd->group_index = LINUX_GROUP_NONE;' "$LTHC" && \
   grep -q 'pd->mem_stage_valid = 0;' "$PDC"; then
    ok "N1. the group's destroy zeroes the shared cursors and the identity clear drops the index, while the descriptor still zeroes the staged brk — a reused slot inherits neither its predecessor's pages-to-be nor an unanswered grant"
else
    bad "N1. the group's slot-reuse clear is gone — the next occupant of a reused slot can resolve its predecessor's group (whose brk_cur maps pages at a break it never asked for), or the live brk stage survives"
fi
span_line="$(grep -nF 'group_heap_base  = stack_base + (uint64_t)stk_pages * 4096;' "$CAPC" | cut -d: -f1)"
# The personality check is bound to the STAMP SITE (the line above the
# stamp), not to the bare pattern: increment 7 added other
# PROC_PERSONALITY_LINUX blocks to cap.c, and a free-standing pattern
# match let the N2 mutation sail through them — a vacuous pass.
if [ -n "$span_line" ] && \
   sed -n "$((span_line - 1))p" "$CAPC" | grep -qF 'if (personality == PROC_PERSONALITY_LINUX) {' && \
   grep -qF '((m.budget_mem_bytes + 4095ULL) / 4096ULL)' "$CAPC" && \
   grep -qF 'linux_group_stamp_memory(pd, group_heap_base, group_heap_limit,' "$CAPC" && \
   grep -qF 'const IMAGE_VBASE: u64 = 0x4000_0000_0000;' "$LPRO" && \
   grep -qF 'const MEM_BUDGET_BYTES: u64 = 64 * 1024;' "$LPRO" && \
   grep -qF 'let heap_base = stack_base + stk_pages * 4096;' "$LPRO"; then
    ok "N2. the span is computed inside the create for LINUX only and stamped onto the GROUP, from the manifest's BUDGET record — and the sidecar's ledger derives the same three numbers from the same record (one truth, mirrored)"
else
    bad "N2. the span's stamp or its twin is gone — the kernel would map brk pages against a span the manifest never declared (or stamp one onto every SLS slot), or the sidecar's ledger would be born at a base the kernel's pages never reach: the two halves silently disagree and every denial-echo check lies"
fi
if grep -qF 'mref = linux_mem_mprotect_refusal(&mreq,' "$DISP" && \
   grep -qF 'if (mref) {' "$DISP" && \
   grep -qF 'mref == -LINUX_EPERM' "$DISP" && \
   grep -qF 'linux_mprotect_refuses_wx(req->prot)' "$LABC" && \
   grep -qF 'if prot & 0x2 != 0 && prot & 0x4 != 0 {' "$LRS" && \
   grep -qF 'const E_PERM: i32 = -12;' "$LRS"; then
    ok "N3. W|X is refused at BOTH ends: the first trap refuses -EPERM before any post, and the sidecar's arm carries the same refusal (CAP_EPERM pinned at -12 so §5.7 delivers 1)"
else
    bad "N3. the W|X refusal exists at one end or neither — one deletion away from a shim that maps write+execute pages, which is the exclusion H2 names (and E_PERM drifted would deliver a different errno than the refusal meant)"
fi
stage_line6=$(grep -n 'cur->mem_stage_valid = 1;' "$DISP" | head -1 | cut -d: -f1)
arm_line6=$(grep -n 'cur->park_syscall = SYS_LINUX_TRAP_RESUME;' "$DISP" | head -1 | cut -d: -f1)
verdict_line=$(grep -n 'linux_mem_brk_verdict(' "$DISP" | head -1 | cut -d: -f1)
deliver_line6=$(grep -n 'k_linux_deliver(cur->pid, ch_r' "$DISP" | head -1 | cut -d: -f1)
map_line=$(grep -n 'linux_group_set_brk(cur, v.new_cur);' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$stage_line6" ] && [ -n "$arm_line6" ] && [ -n "$verdict_line" ] && \
   [ -n "$deliver_line6" ] && [ -n "$map_line" ] && \
   [ "$stage_line6" -lt "$arm_line6" ] && \
   [ "$verdict_line" -gt "$deliver_line6" ] && [ "$map_line" -gt "$verdict_line" ] && \
   grep -qF 'if (v.malformed) {' "$DISP" && \
   grep -qF 'linux_group_set_brk(cur, v.new_cur);' "$DISP"; then
    ok "N4. the brk seam holds: staged BEFORE the arm, the verdict consulted only AFTER the reply is delivered, malformed grants refused before any map, and the group's break recorded only on a fully-mapped grant"
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
if grep -qF 'if want > brk.cur && want <= brk.mtop {' "$LRS" && \
   grep -qF 'Answer::Reply(brk.cur, STATUS_OK)' "$LRS" && \
   grep -qF 't.brk_cur = heap_base;' "$LRS"; then
    ok "N7. the sidecar's brk arm grants only inside the span CAPPED by the map cursor and answers the UNCHANGED current break otherwise — Linux's own failure shape, a value not an errno, so no libc's brk wrapper can be surprised, and never a page a granted map owns"
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
   grep -qF '#define SIDECAR_MANIFEST_MAX_ARGV  12' "$CAPH" && \
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
   grep -qF 'pub const MAX_ARGV: usize = 12;' "$MANR" && \
   grep -qF 'pub const MAX_ENVP: usize = 16;' "$MANR" && \
   grep -qF 'pub const ARGENV_MAX_STR_BYTES: usize = 512;' "$MANR" && \
   grep -qF 'recs.push((TAG_ARGENV, p));' "$MANR" && \
   grep -qF 'TAG_ARGENV => {' "$MANR" && \
   grep -qF 'pub const LINUX_DEFAULT_ENV: &[&str] = &["PATH=/bin", "HOME=/root"];' "$LRS" && \
   grep -qF 'argenv: Some(ArgEnv::new(&[PROBE_NAME], LINUX_DEFAULT_ENV)),' "$LPRO" && \
   grep -qF 'argenv: Some(ArgEnv::new(&[CAND_NAME], LINUX_DEFAULT_ENV)),' "$CAND" && \
   grep -qF 'argenv: Some(ArgEnv::new(argv, LINUX_DEFAULT_ENV)),' "$LFU"; then
    ok "O7. the Rust format reference carries the record with the kernel's bounds and emits it, the environment crosses from one shared default list, and all three Linux spawn sites author the record (firstuser with the gate's command line or v1's { name }, probe and candidate — the pair stays a pair)"
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
if grep -qF 'sig.act[args[0] as usize] = act;' "$LRS" && \
   grep -qF 'dispo(sig, args[1]) == DISPO_HANDLER {' "$LRS" && \
   grep -qF 'dispo(sig, sig_no as u64) == DISPO_HANDLER {' "$LRS" && \
   grep -qF 'if sig.tid == 0 || args[0] as u32 != sig.tid as u32 {' "$LRS" && \
   grep -qF 'sig.pending |= 1u64 << (args[1] - 1);' "$LRS"; then
    ok "R5. the seam's honesty lines hold: rt_sigaction RECORDS the whole 32-byte act — handler address, flags, mask and restorer, returned verbatim by oldact — but never invokes it, tkill's own delivery and the unblock reveal both refuse a handler disposition ENOSYS by name BEFORE any action (no signal frame/rt_sigreturn exists), tkill's tid is the set_tid_address echo at u32 width (ESRCH otherwise — never silently re-targeted), and a blocked signal PENDS so the unblock arm is where it delivers"
else
    bad "R5. one of the seam's honesty lines is gone — a recorded handler could be invoked with no frame to return through (or silently turned into a default kill the caller never asked for), a tid mismatch could kill a task that never asked, or a blocked raise() would be lost outright instead of pending"
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
   grep -qF 'fn fmt_mmap_refusal_line' "$LRS" && \
   grep -qF 'b"ENOSYS mmap(len="' "$LRS" && \
   grep -qF 'E_NOSYS, args[1], args[2], args[3]' "$LRS"; then
    ok "R8. the SIGABRT reading's instrument holds: the workload's own console bytes ride its write lines (len + a 24-byte excerpt, child-attributed) and mmap refusals carry their request (len/prot/flags) — 'alloc failed after ENOSYS' is read off the E7 stream, never inferred from a merged console drain whose order is the flush, not the writer"
else
    bad "R8. the attribution is gone — the alloc-failure text would exist only in the merged console drain (any child of the environment could have written it) and the mmap refusal goes back to a bare ENOSYS with no request to name (the proven chain becomes an inference again)"
fi
if ! grep -qF 'cmp rax, 131' "$ASM" && \
   ! grep -qF '.do_sls_svc_crash' "$ASM" && \
   grep -qF '131 (sigaltstack in the Linux census) is deliberately NOT routed here.' "$ASM" && \
   grep -qF 'case SYS_SLS_SVC_CRASH:' "$DISP" && \
   grep -qF 'LINUX_SIGALTSTACK => {' "$LRS" && \
   grep -qF 'SS_DISABLE' "$LRS"; then
    ok "R9. sigaltstack (131) is NOT in the asm number chain — the personality branch, not the number alone, decides it: the real firstuser's query reaches the sidecar's 131 arm (old stack_t = SS_DISABLE), its install is a no-op, and Rust std falls through to the guard-page mmap+mprotect; SLS still reaches SYS_SLS_SVC_CRASH through do_syscall's own arm"
else
    bad "R9. the asm fast path claims 131 again (or the sidecar's SS_DISABLE answer/SLS arm is gone) — a Linux sigaltstack(NULL,&old) would become an SLS svc_crash, the old stack_t would stay zeroed, std would skip the guard-page mmap+mprotect, and every other clause could stay green while the real firstuser's guard call silently vanished"
fi

# ─── S. poll (7) — readiness and timed wakeups (E7 increment 24) ───────────
# The firstuser's last ENOSYS answer is now a REAL call. The kernel copies
# the caller's pollfd array out and the rewritten array back into the SAME
# span; the sidecar scans the task's fd table and the console, answers a
# ready or zero-timeout poll at once, and PARKS a timed or infinite poll
# until readiness or the deadline. A timed-out poll answers zero revents —
# never invented readiness — and a blocking console read parks the same way.
# Every clause below is a way that becomes a silent lie while the rest of
# the guard stays green: an array one side never carries, the write clamp
# eating poll's timeout, a parked request nothing ever wakes, a timeout
# that never expires, or a pure half no host test drives.
if grep -qF '#define LINUX_POLL  7u' "$LABH" && \
   grep -qF '#define LINUX_POLL_MAX_FDS 16u' "$LABH" && \
   grep -qF '#define LINUX_POLLFD_BYTES 8u' "$LABH" && \
   grep -qF 'if (num == LINUX_POLL) {' "$LABC" && \
   grep -qF 'p.len = (uint32_t)(nfds * LINUX_POLLFD_BYTES);' "$LABC" && \
   grep -qF '} else if (num == LINUX_POLL) {' "$LABC" && \
   grep -qF 'n % LINUX_POLLFD_BYTES == 0' "$LABC"; then
    ok "S. poll's crossings are named pure contracts at the kernel end — the capped array IN (write plan), the SAME array back OUT (reply plan), and a reply shape that keeps the ready count inside nfds"
else
    bad "S. one half of poll's byte crossing is gone — without the IN plan revents cannot be computed from the caller's array, without the OUT plan the rewritten array never lands, and without the shape rule a ready count can contradict it"
fi
if grep -qF 'if (num == LINUX_WRITE && plan.len) largs[2] = plan.len;' "$DISP"; then
    ok "S1. the description's clamp is WRITE-only — poll's args[2] is its TIMEOUT, so the array's byte length must never ride that word (an unconditional clamp would answer a 5 s poll on the array's byte count — 24 ms for three fds — not on its timeout)"
else
    bad "S1. poll's timeout word is being overwritten by the write clamp (or the write clamp is gone) — a 5 s poll would return after ~24 ms with nothing ready, its timeout silently ignored"
fi
if grep -qF 'const PENDING_READ: u8 = 1;' "$LRS" && \
   grep -qF 'const PENDING_POLL: u8 = 2;' "$LRS" && \
   grep -qF 'LINUX_POLL => poll_answer(fds, console, payload, args, now_ns,' "$LRS" && \
   grep -qF 'return Answer::ReadWait(want);' "$LRS" && \
   grep -qF 'Answer::PollWait(arr, len, poll_deadline(now_ns, timeout_ms))' "$LRS" && \
   grep -qF 'poll_expired(deadline, now_ns)' "$LRS" && \
   grep -qF 'deadline != 0 && now_ns >= deadline' "$LRS" && \
   grep -qF 'tasks()[i].pending = PENDING_POLL;' "$LRS" && \
   grep -qF 'tasks()[i].pending = PENDING_READ;' "$LRS"; then
    ok "S2. the sidecar's waiting half is wired — a console read with no input returns ReadWait, a timed poll returns PollWait with its deadline, service PARKS both, re-scans on every pass, and answers on readiness or expiry"
else
    bad "S2. the sidecar's park/wake half is gone or degraded — a blocking read would answer EAGAIN, a timed poll would answer 0 immediately, or a parked request would sit with nothing to resolve it"
fi
if grep -qF 'let timeout = crate::linux::next_wait_ns(&RealKernel);' user/sidecar/src/entry.rs && \
   grep -qF 'pub fn next_wait_ns<K: Kernel>(k: &K) -> u64' "$LRS" && \
   grep -qF 't.pending == PENDING_POLL && t.pending_deadline != 0' "$LRS"; then
    ok "S3. the entry loop waits with the nearest finite poll deadline — a timed poll wakes the parked sidecar at its own timeout instead of waiting for an unrelated channel, and a read/forever-poll keeps the old block-forever shape"
else
    bad "S3. the quiesce wait no longer carries poll's deadline — a 5 s poll would sleep until some other channel happened to wake the loop, and its timeout would be decided by unrelated traffic"
fi
if grep -qF 'linux_fd_write_plan(LINUX_POLL, a)' "$PHUT" && \
   grep -qF 'linux_fd_reply_plan(LINUX_POLL, a)' "$PHUT" && \
   grep -qF 'linux_reply_shape_ok(LINUX_POLL, 2, 3 * LINUX_POLLFD_BYTES)' "$PHUT" && \
   grep -qF 'LINUX_POLL_MAX_FDS + 1' "$PHUT" && \
   grep -qF '0x00007FFFFFFFFFF8ULL' "$PHUT"; then
    ok "S4. the host test drives both plans against the REAL kernel/linux_abi.c — the in/out crossings, nfds 0, the cap, the NULL array, the span crossing the user max, and the reply shape"
else
    bad "S4. the host test no longer drives poll's plans — the crossings and refusals would be argued from this guard's greps alone, every branch reachable only at boot"
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
   grep -qF 'uint64_t rax = linux_syscall_apply_reply(' "$CANC" && \
   grep -qF 'skind == LINUX_STAGE_STAT' "$CANC" && \
   grep -qF 'skind == LINUX_STAGE_DENTS' "$CANC" && \
   grep -qF 'linux_dirent_pack(pay, n, saux,' "$CANC" && \
   grep -qF 'linux_user_copy_in(pay, pd->cr3, sva, n) != 0' "$CANC"; then
    ok "T3. the deliver accepts exactly three shapes — 72 (a SUCCESS with an owed payload refused -EPROTO), 72+n within the staged clamp with a matching echo/status/ret, and CAP_ERR_PROTO otherwise — apply first, then the payload's KIND decides what crosses (RAW verbatim, STAT serialized, DENTS packed with the cursor advanced only by what was packed), then rax"
else
    bad "T3. a shape arm is gone — a payload would be copied for a number that staged none, beyond the clamp, past a refusal, or with a ret its number contradicts (the one-truth rule with bytes attached)"
fi
if grep -qF 'int require_rw, int accept_soft)' "$LABC" && \
   grep -qF 'if (require_rw && !(e & LAB_PTE_RW)) return -LINUX_EFAULT;' "$LABC" && \
   grep -qF 'static int lab_walk_leaf_rw(' "$LABC" && \
   grep -qF 'return lab_walk_leaf(root, va, leaf_out, 1, 0);' "$LABC" && \
   grep -qF '#define LAB_PTE_RW' "$LABC" && \
   grep -qF 'Phase 1 — validate the WHOLE span' "$LABC" && \
   grep -qF 'Phase 2 — write.' "$LABC"; then
    ok "T4. the copy-IN validates (present, USER, WRITABLE at every level) the WHOLE span before writing a byte — the sidecar can never launder a write through a page the child could not have written, and a refusal mutates nothing"
else
    bad "T4. the writable check or the two-phase order is gone — a read-only page under a payload becomes writable-by-sidecar, or a span half-writes before its second page refuses"
fi
if grep -qF 'ReplyBytes(u64, usize, [u8; FD_PAYLOAD_CAP]),' "$LRS" && \
   grep -qF 'if !payload_bound(num, &args, len) || len > bytes.len() {' "$LRS" && \
   grep -qF 'reply payload mismatch -> -EPROTO' "$LRS" && \
   grep -qF 'reply_bytes(k, ch_w, rr.tag, num, ret, &bytes[..len]);' "$LRS" && \
   grep -qF 'old_act_bytes(sig, args[0])' "$LRS" && \
   grep -qF 'fn payload_bound(num: u64, args: &[u64; 6], len: usize) -> bool {' "$LRS"; then
    ok "T5. the sidecar's twin bound — payload_bound derives the per-number clamp from the same words the kernel's reply plan stages (the sig class's exact size, stat's compact 24, read's count, getdents' cap) or the answer goes out as num=0 → -EPROTO, the old disposition is captured BEFORE any mutation, and only service() sends the bytes (neither end load-bearing alone)"
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

capfn="$(awk '/^static void proc_capture_entry_regs\(struct CapParkCtx\* ctx\) \{/{f=1} f{print} f&&/^}/{exit}' "$PDC")"
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

# ─── V: the anonymous-map seam (E7 increment 16, §5.4) ────────────────────
# §10.10's last anonymous wall lands: the two mmap requests that killed
# the firstuser's allocator are answered from the same budget span brk
# lives in, at both ends, with neither end load-bearing alone. The seam
# is the brk pattern over four staged words: a first-trap ladder over
# the REQUEST's own properties (so no channel is posted for a shape
# that cannot be answered), a verdict at the reply that re-checks the
# grant against the kernel's OWN stamped span and OWN two cursors, and
# an apply that replaces (MAP_FIXED over the brk heap's first page —
# the one path here that edits a translation ring-3 HAS run on, hence
# invlpg). Every clause below is a way this becomes the silent lie it
# was built to prevent: a W|X map granted, a cursor that hands the same
# arena out twice, a brk growth mapping over an arena, a replace that
# leaks the old frame or runs on a stale TLB entry, a fixed map whose
# frames carry the pool's last tenant's bytes, or a ledger and a kernel
# that each believe the other side capped.
if grep -qF 'struct LinuxMmapReq {' "$LABH" && \
   grep -qF 'int linux_mem_mmap_refusal(const struct LinuxMmapReq *req,' "$LABH" && \
   grep -qF 'linux_mem_mmap_verdict(uint64_t heap_base, uint64_t heap_limit,' "$LABH" && \
   grep -qF '#define LINUX_MAP_ANONYMOUS 0x0020u' "$LABH" && \
   grep -qF '#define LINUX_MAP_V1_ALLOW' "$LABH" && \
   grep -qF 'int linux_mem_mmap_refusal(const struct LinuxMmapReq *req,' "$LABC" && \
   grep -qF 'linux_mem_mmap_verdict(uint64_t heap_base, uint64_t heap_limit,' "$LABC"; then
    ok "V. the mmap seam is a named pure contract, declared where the dispatcher, the sidecar twin and the host test all read it: the request struct, the first-trap ladder, the verdict, and the closed flag allowlist"
else
    bad "V. one of the mmap contracts is gone or un-declared — the first trap would have no refusal to consult (a W|X or file-backed map posts), or the verdict no pure function the host test can drive"
fi
mmap_ref=$(grep -nF 'mref = linux_mem_mmap_refusal(&mreq,' "$DISP" | head -1 | cut -d: -f1)
mmap_stage=$(grep -nF 'cur->mem_stage_a3    = largs[3];' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$mmap_ref" ] && [ -n "$mmap_stage" ] && [ "$mmap_ref" -lt "$mmap_stage" ] && \
   sed -n "$((mmap_ref + 1)),$((mmap_ref + 18))p" "$DISP" | grep -qF 'refused before post' && \
   sed -n "$((mmap_ref + 1)),$((mmap_ref + 18))p" "$DISP" | grep -qF -- '(uint64_t)(int64_t)mref;' && \
   grep -qF 'cur->mem_stage_a1    = 0;' "$DISP" && \
   grep -qF 'cur->mem_stage_a2    = 0;' "$DISP"; then
    ok "V1. the FIRST TRAP consults the ladder before any post — the refusal returns in the task's own units with the request's words on the line, and a pass stages the WHOLE ask (addr/len/prot/flags) after the stage's three-word clear"
else
    bad "V1. the first-trap ladder or the four-word stage is gone — a W|X, file-backed or out-of-span map would post (the sidecar twin then being the only wall), or the verdict would re-check a grant against words the FIRST trap never saw"
fi
mmap_v=$(grep -nF 'v = linux_mem_mmap_verdict(' "$DISP" | head -1 | cut -d: -f1)
if [ -n "$mmap_v" ] && \
   sed -n "$((mmap_v - 3)),$((mmap_v + 6))p" "$DISP" | grep -qF 'linux_group_mmap_top(cur)' && \
   grep -qF 'v.new_mmap_top);' "$DISP" && \
   grep -qF 'linux_group_mmap_top(cur), &mreq, rax);' "$DISP" && \
   grep -qF '(-LINUX_ENOMEM);' "$DISP" && \
   grep -qxF '                        } else if (snum == LINUX_MMAP) {' "$DISP" && \
   ! grep -qF '                            } else if (snum == LINUX_MMAP) {' "$DISP"; then
    ok "V2. the REPLY verdict runs with the kernel's OWN span and BOTH cursors (brk_cur, mmap_top) — at the snum level the brk chain is chained FROM (an else hung inside the brk verdict's map-pages arm compiles clean and is unreachable for LINUX_MMAP: the measured failure), a malformed grant answers -ENOMEM with nothing mapped, and a success moves the kernel's cursor by the verdict's own number"
else
    bad "V2. the verdict call, its cursor arguments or the malformed arm is gone — a forged or drifted grant would map pages the kernel never re-checked, or the two cursors would stop converging"
fi
inv_upc=$(grep -nF 'void user_invlpg(uint64_t vaddr)' "$UPC" | head -1 | cut -d: -f1)
if [ -n "$inv_upc" ] && \
   sed -n "$((inv_upc + 1))p" "$UPC" | grep -qF 'invlpg (%0)' && \
   grep -qF 'void user_invlpg(uint64_t vaddr);' "$UPH" && \
   grep -qF 'if (v.replace)' "$DISP" && \
   grep -qF 'user_invlpg(va);' "$DISP" && \
   grep -qF 'free_physical_ram_frame_for_partition' "$DISP"; then
    ok "V3. MAP_FIXED replaces honestly — the old page's frame is read back and returned to the partition, the one stale-translation path (a PRESENT leaf ring-3 ran on) shoots its TLB entry down through the named invlpg, and the new protection is installed per prot"
else
    bad "V3. the replace walk, the frame return or the invlpg is gone — a replace would leak the old frame and zero live bytes, or ring-3 would keep running on the stale translation the guard page replaced"
fi
if grep -qF 'g->mmap_top = heap_limit;' "$LTHC" && \
   grep -qF 'g->mmap_top = 0;' "$LTHC" && \
   grep -qF 'uint64_t mmap_top;' "$LTHH"; then
    ok "V4. both cursors are stamped and cleared on the GROUP — the stamp births mmap_top at the span's limit beside brk_cur, destroy zeroes it beside the other fields, and linux_thread.h declares it"
else
    bad "V4. the group cursor's stamp, clear or field is gone — a reused group would inherit its predecessor's cursor (a grant landing on a page the last occupant's map owned) or a LINUX task would have no cursor to cap brk by"
fi
mmap_arm=$(grep -nF '        9 => {' "$LRS" | head -1 | cut -d: -f1)
if [ -n "$mmap_arm" ] && \
   sed -n "$((mmap_arm + 1)),$((mmap_arm + 60))p" "$LRS" | grep -qF 'MAP_ALLOW' && \
   sed -n "$((mmap_arm + 1)),$((mmap_arm + 60))p" "$LRS" | grep -qF 'checked_add(0xFFF)' && \
   sed -n "$((mmap_arm + 1)),$((mmap_arm + 60))p" "$LRS" | grep -qF 'checked_sub(len_r)' && \
   sed -n "$((mmap_arm + 1)),$((mmap_arm + 60))p" "$LRS" | grep -qF 'Answer::Reply(0, E_NOMEM)' && \
   grep -qF 'mtop: u64,' "$LRS" && \
   grep -qF 't.brk_mtop = heap_limit;' "$LRS" && \
   grep -qF 'want > brk.cur && want <= brk.mtop' "$LRS" && \
   grep -qF 'tasks()[i].brk_mtop = mem.mtop;' "$LRS"; then
    ok "V5. the sidecar's 9 arm is the ladder's twin and the ledger's policy — the closed allowlist, the overflow-checked page-round, the descending cursor with its checked subtract, ENOMEM when the gap cannot fit, mtop born at the span's top, survived across the reply, and brk's ceiling capped by it (neither end load-bearing alone)"
else
    bad "V5. the arm, the ledger's mtop or brk's cap is gone — the cursor could hand the same arena out twice, land under the break, or a brk growth could map over an arena the other side granted"
fi
if grep -qF 'linux_mem_mmap_refusal(&req, BASE, LIMIT)' "$PHUT" && \
   grep -qF 'linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,' "$PHUT" && \
   grep -qF 'linux_mem_brk_verdict(BASE, LIMIT, LIMIT - 0x1000,' "$PHUT"; then
    ok "V6. the host test drives the ladder, the verdict AND the brk cap against the REAL linux_abi.c — every branch reachable without a boot (run_all compiles and runs it)"
else
    bad "V6. the host test no longer drives the mmap seam — every refusal and verdict branch would be argued from these greps alone, reachable only at boot"
fi
if grep -qF '.Lmmap:' "$FIX" && \
   grep -qF 'E7MEM-MMAP-OK' "$FIX" && \
   grep -qF 'mov     $399, %eax' "$FIX" && \
   grep -qF 'mov     $400, %eax' "$FIX" && \
   grep -qF 'mov     $401, %eax' "$FIX" && \
   grep -qF 'mov     $402, %eax' "$FIX" && \
   grep -qF 'mov     $403, %eax' "$FIX"; then
    ok "V7. the fixture carries the E7MEM-MMAP block — the arena grant (aligned, in-span), the zero-read, the bytes out FROM THE MAPPED PAGE, the FIXED guard's exact echo, the W|X twin and the budget shape, each with its own marker (399–403, asserted absent at boot)"
else
    bad "V7. the fixture's map block or its markers are gone — the anonymous-map seam would have no on-target proof, and a regression would read as a quiet run"
fi
if grep -qF 'fn hex_into' "$LRS" && \
   grep -qF ' ok mmap -> 0x' "$LRS" && \
   grep -qF 'fn fmt_mmap_refusal_line' "$LRS" && \
   grep -qF 'b"EPERM mmap(len="' "$LRS" && \
   grep -qF 'b"ENOSYS mmap(len="' "$LRS"; then
    ok "V8. the instrument carries every mmap answer — a grant shows the address the ledger gave (hex: an address is unreadable in decimal), every refusal shows the request's words with the errno it became, and the increment-15 ENOSYS line is unchanged behind it"
else
    bad "V8. an mmap answer line is gone — the run's proof would be a reconstruction again (the increment-14/15 instrument rule)"
fi

# ─── W. E7 increment 20: the task in the record, its state in the payload ──
# B6/B7 were the named blockers: the record's kind existed but nothing filled
# it, and the payload SKIPPED the Linux task in all three loops. These
# clauses hold the three halves that make the tenant checkpointable — the
# identity the manager names, the kernel facts the spawn stamps, and the
# bytes + permissions the capture files — plus the one form the blob can
# honestly resume (USER), refused by name otherwise. The host-test clause
# also holds the v2 replay and the corrupt soft-permission refusals that
# keep old blobs readable without admitting v3-only states.
EPH=kernel/env_payload.h
EPC=kernel/env_payload.c
ECKH=kernel/env_ckpt.h
ECKC=kernel/env_ckpt.c
IEM=user/init/src/env_manager.rs
ENVP=user/proto/src/env_proto.rs
EPHUT=tests/env_payload_host_test.c
ECKHUT=tests/env_ckpt_host_test.c
if grep -qE '#define ENV_PAYLOAD_REC_VERSION[[:space:]]+5u' "$EPH" && \
   grep -qF '#define EP_PAGE_ENTRY_BYTES 16u' "$EPH" && \
   grep -qE '#define EP_PAGE_PERM_SOFT[[:space:]]+0x8u' "$EPH" && \
   grep -qF 'uint64_t linux_regs[20];' "$EPH" && \
   grep -qF 'uint64_t linux_args[6]; /* rdi, rsi, rdx, r10, r8, r9 at the capture */' "$EPH" && \
   grep -qF 'uint64_t fs_base;' "$EPH" && \
   grep -qF 'uint64_t brk_cur;' "$EPH" && \
   grep -qF 'uint64_t mmap_top;' "$EPH" && \
   grep -qF 'uint64_t heap_limit;' "$EPH" && \
   grep -qF 'uint64_t stack_bottom;' "$EPH" && \
   grep -qF 'uint64_t image_hi;' "$EPH" && \
   grep -qF 'uint32_t group_id;' "$EPH" && \
   grep -qF 'uint16_t thread_index;' "$EPH" && \
   grep -qF 'uint8_t  leader;' "$EPH" && \
   grep -qF 'uint64_t futex_uaddr;' "$EPH" && \
   grep -qF 'uint32_t futex_val;' "$EPH" && \
   grep -qF 'uint32_t futex_ticks;' "$EPH" && \
   grep -qF '#define ENV_PAYLOAD_FORM_USER    2u' "$EPH" && \
   grep -qF '#define ENV_PAYLOAD_FORM_FUTEX   4u' "$EPH"; then
    ok "W1. the payload is v5: EnvPayloadTask carries the Linux task's FULL context (20-word TaskContext), its six argument registers, fs_base, both cursors, the heap and stack ranges, the image span, its group id/index/leader, and a FORM_FUTEX barrier key/value/remaining timeout; every page index entry is 16 bytes with a permission word including the soft PROT_NONE state"
else
    bad "W1. the Linux half, group identity or futex-wait half of EnvPayloadTask is gone — the capture could file pages a restore cannot map, two siblings would be indistinguishable, a barrier wait would have no key to re-enqueue, or an older stride would be misread task by task"
fi
if grep -qF 'static int ep_linux_page(const struct EnvCkptRecord* rec, uint32_t t,' "$EPC" && \
   grep -qF 'if (tk->stack_bottom && vaddr >= tk->stack_bottom &&' "$EPC" && \
   grep -qF 'if (linux_group_image_lo(pd) && vaddr >= linux_group_image_lo(pd) &&' "$EPC" && \
   grep -qF 'if (linux_group_heap_base(pd) && vaddr >= linux_group_heap_base(pd) &&' "$EPC" && \
   grep -qF 'return ep_in_image_window(vaddr) || ep_in_regions(rec, vaddr);' "$EPC" && \
   grep -qF 'if (!ep_task_page(rec, t, pds[t], v)) continue;' "$EPC" && \
   grep -qF 'static uint32_t ep_perm_of(uint64_t e)' "$EPC" && \
   grep -qF 'ep_perms[pi] = perm;' "$EPC"; then
    ok "W2. the capture walks the Linux CR3 with a PER-TASK predicate — its own image span, the record's own stack bounds and its budget span (never the parent's image window, which the child's cloned table also contains), resolving each page's permissions from the live PTE"
else
    bad "W2. the Linux page predicate or the per-page permission resolve is gone — the walk would either miss the tenant's bytes or capture the PARENT's cloned image pages under the child, and the blob would carry no permissions to check"
fi
if [ "$(awk '/^uint32_t env_payload_capture_all\(void\) \{/,/^\}/' "$EPC" | grep -c 'kind == ENV_CKPT_TASK_LINUX) continue')" -eq 0 ] && \
   grep -qF 'struct ProcessDescriptor* pd = ep_find_ckpt_task(&rec->tasks[t]);' "$EPC" && \
   grep -qF 'static struct ProcessDescriptor* ep_find_group_member' "$EPC" && \
   grep -qF 'missing ? "named task has no live process" : "no tasks"' "$EPC"; then
    ok "W3. the capture no longer skips the Linux task and resolves a group entry by (group id, thread index), so two siblings with the same name are distinct; every named task must resolve, and a checkpoint taken before the tenant is spawned skips the ENVIRONMENT by name"
else
    bad "W3. the capture still skips the Linux kind, or resolves group siblings by name (or names a missing task without saying so) — a checkpoint of a live gate environment would silently omit or overwrite a thread while the record claims it"
fi
if grep -qF 'case EP_REFUSE_PERM:' "$EPC" && \
   grep -qF 'case EP_REFUSE_FORM:' "$EPC" && \
   grep -qF 'if ((live_perm & EP_PAGE_PERM_MASK) !=' "$EPC" && \
   grep -qF 'ep_refuse(EP_REFUSE_PERM, t, pi);' "$EPC" && \
   grep -qF 'if (tk.form != ENV_PAYLOAD_FORM_USER &&' "$EPC" && \
   grep -qF 'tk.form != ENV_PAYLOAD_FORM_ENTRY &&' "$EPC" && \
   grep -qF 'tk.form != ENV_PAYLOAD_FORM_FUTEX) {' "$EPC" && \
   grep -qF 'if (tk.form == ENV_PAYLOAD_FORM_FUTEX) {' "$EPC" && \
   grep -qF 'linux_futex_wait_enqueue(fgid, tk.futex_uaddr, pd->tid,' "$EPC" && \
   grep -qF 'pd->has_ring3_ctx = 1;' "$EPC" && \
   grep -qF 'linux_group_set_mmap_top(pd, tk.mmap_top);' "$EPC"; then
    ok "W4. the restore proves the page's live permissions against the captured ones before a byte moves, resumes the resumable Linux forms (USER and increment 21's held ENTRY), re-enqueues FORM_FUTEX with its expected word and relative deadline instead of resuming it, refuses PARKED/RUNNING by name, and carries the captured context, fs_base and both cursors onto the group"
else
    bad "W4. the restore's Linux arm is gone — a page could be poured into a map with different permissions, a parked or futex wait's stale context would be resumed, a non-held task could be poured over, or the task would restart at its entry"
fi
if grep -qF 'pub const LINUX_TASK_NAME: &str = "e7.gate.user.0";' "$ENVP" && \
   grep -qF 'r.set_task(2, LINUX_TASK_NAME, TASK_LINUX);' "$IEM" && \
   grep -qF 'let gate = cfg!(feature = "e7_gate") && index == 1;' "$IEM" && \
   grep -qF 'pub const GATE_TASK_NAME: &str = aerosls_proto::env_proto::LINUX_TASK_NAME;' "$LFU" && \
   grep -qF 'if (rec->tasks[i].entry == 0 &&' "$ECKC" && \
   grep -qF 'rec->tasks[i].kind != ENV_CKPT_TASK_LINUX)' "$ECKC" && \
   grep -qF 'int env_ckpt_linux_task_stamp(' "$ECKC" && \
   grep -qF '(void)env_ckpt_linux_task_stamp(parent->partition_id, parent->name,' "$CAPC" && \
   grep -qF 'stack_base + (uint64_t)stk_pages * 4096,' "$CAPC" && \
   grep -qF 'bib_n_caps);' "$CAPC"; then
    ok "W5. registration is settled: the manager names the third task from the SAME gate bit that selected the manifest (so a replay's create names the same list), the sidecar spawns that one name, the record accepts a Linux entry of 0 until the spawn, and the kernel stamps entry/stack/caps at the create — the only place they are known"
else
    bad "W5. the registration chain has a hole — the record would name a task nobody spawns (or vice versa), a Linux slot's not-yet-spawned entry would refuse the whole registration, or the stamped entry/stack/caps would never reach the record"
fi
if grep -qF 'LINUX: the gate environment (sidecar + Linux tenant) captures as one payload' "$EPHUT" && \
   grep -qF 'LINUX: the full ring3_ctx — rip, rsp and the six argument registers — is the captured one' "$EPHUT" && \
   grep -qF 'PERM: a captured RX image page that is writable at the destination refuses the whole pour' "$EPHUT" && \
   grep -qF 'GROUP: a sidecar plus a two-thread Linux group captures as one payload' "$EPHUT" && \
   grep -qF 'BARRIER: both tasks carry the same address and expected word' "$EPHUT" && \
   grep -qF 'BARRIER: both waits are re-enqueued before the group is released' "$EPHUT" && \
   grep -qF 'BARRIER: both descriptors are BLOCKED in the futex resume form, not runnable' "$EPHUT" && \
   grep -qF 'RELEASE: one call releases both held members of the restored group' "$EPHUT" && \
   grep -qF 'BARRIER-REFUSE: a mixed futex/runnable group is refused by name before any pour' "$EPHUT" && \
   grep -qF 'BARRIER-REFUSE: one barrier address with two expected words is refused before any pour' "$EPHUT" && \
   grep -qF 'uint32_t linux_group_release_restored(uint32_t group_id);' "$LTHH" && \
   grep -qF 'uint32_t linux_group_release_restored(uint32_t group_id) {' "$LTHC" && \
   grep -qF 'linux_group_release_restored(' kernel/env_service.c && \
   grep -qF 'GROUP: the leader is (group 101, index 0), owns the image/stack/heap pages' "$EPHUT" && \
   grep -qF 'GROUP: the sibling is a distinct (group 101, index 1) with no duplicate shared pages' "$EPHUT" && \
   grep -qF 'GROUP: both thread contexts are installed into their own descriptors' "$EPHUT" && \
   grep -qF "GROUP: the leader's shared cursors are restored once, and the sibling does not overwrite them" "$EPHUT" && \
   grep -qF "CHURN: the exiting clone's record entry is dropped by group/index" "$EPHUT" && \
   grep -qF 'CHURN: a later capture still succeeds after the cloned thread exits' "$EPHUT" && \
   grep -qF 'CHURN: the recaptured payload no longer names the dead group thread' "$EPHUT" && \
   grep -qF 'env_ckpt_linux_group_thread_drop(' "$DISP" && \
   grep -qF 'DUP: one vaddr twice with different bytes is refused by name, before any pour' "$EPHUT" && \
   grep -qF 'DUP: the refusal names the first entry and the duplicate' "$EPHUT" && \
   grep -qF 'V4: a v4 blob is refused by format before any pour (the v5 task stride is not its stride)' "$EPHUT" && \
   grep -qF 'SOFT+USER: the restore refuses the contradictory permission before a byte moves' "$EPHUT" && \
   grep -qF 'SOFT+WX: a writable+executable soft page is refused by format, not poured' "$EPHUT" && \
   grep -qF 'SOFT+USER: the refusal names the permission rule (a=8), not a torn index CRC' "$EPHUT" && \
   grep -qF 'SOFT+WX: the refusal names the permission rule (a=8), not a torn index CRC' "$EPHUT" && \
   [ "$(grep -cF 'CHECK(env_payload_last_refusal().a == 8,' "$EPHUT")" -eq 2 ] && \
   grep -qF 'FORM: a parked Linux tenant is refused by name' "$EPHUT" && \
   grep -qF 'the stamp fills the Linux slot' "$ECKHUT" && \
   grep -qF 'a replay whose create names the same three tasks IS the same environment' "$ECKHUT"; then
    ok "W6. both host tests drive the increment: the payload round trip (bytes, perms, per-thread group contexts and shared-page ownership), the sibling-exit churn recapture, the futex barrier capture/re-enqueue/release, the duplicate-page refusal, the v4-stride refusal, the corrupt soft-permission refusals with both a=8 operand assertions, and the record layer's stamp/identity clauses — each claim runs against the real kernel file, not a model of it"
else
    bad "W6. a host-test clause for the Linux payload is missing — the group capture/restore or churn recapture, the futex barrier or coordinated release, the duplicate-page refusal, the v4-stride refusal, a corrupt soft-permission refusal or the record stamp would be a claim no test runs"
fi

# ─── X. E7 increment 25: munmap, the memory class's last seam ─────────────
# The contract (X), the first trap before any post (X1), the reply's
# page-table walk and frame return (X2), the sidecar's twin (X3), the
# host test's branches (X4), the probe's on-target proof (X5) and the
# instrument lines (X6): each is a claim the seam rests on.
if grep -qF 'struct LinuxMunmapReq {' kernel/linux_abi.h && \
   grep -qF 'int linux_mem_munmap_refusal(const struct LinuxMunmapReq *req,' kernel/linux_abi.h && \
   grep -qF 'linux_mem_munmap_verdict(uint64_t heap_base, uint64_t heap_limit,' kernel/linux_abi.h && \
   grep -qF 'int linux_mem_munmap_refusal(const struct LinuxMunmapReq *req,' kernel/linux_abi.c && \
   grep -qF 'linux_mem_munmap_verdict(uint64_t heap_base, uint64_t heap_limit,' kernel/linux_abi.c; then
    ok "X. the munmap seam is a named pure contract at both ends — the request, the first-trap refusal (zero/overflowing length, unaligned address, outside the stamped span, below the current break) and the reply verdict that re-checks a success against the kernel's own span and names the page range"
else
    bad "X. the munmap contract is missing — the kernel would have no named refusal or verdict to reason about, and the seam would be a page-table walk with no policy"
fi
munmap_ref=$(grep -nF 'mref = linux_mem_munmap_refusal(&mreq,' kernel/syscall_dispatch.c | head -1 | cut -d: -f1)
if [ -n "$munmap_ref" ] && \
   sed -n "$((munmap_ref - 3)),$((munmap_ref + 26))p" kernel/syscall_dispatch.c | grep -qF 'refused before post' && \
   sed -n "$((munmap_ref - 3)),$((munmap_ref + 26))p" kernel/syscall_dispatch.c | grep -qF 'return (uint64_t)(int64_t)mref;' && \
   sed -n "$((munmap_ref - 3)),$((munmap_ref + 26))p" kernel/syscall_dispatch.c | grep -qF 'cur->mem_stage_num   = num;' && \
   sed -n "$((munmap_ref - 3)),$((munmap_ref + 26))p" kernel/syscall_dispatch.c | grep -qF 'cur->mem_stage_a1    = largs[1];'; then
    ok "X1. munmap's first trap runs before any post: the refusal logs its words and returns its errno on the line; a pass stages addr and len for the reply's verdict"
else
    bad "X1. the munmap first trap is gone or no longer precedes the post — a malformed range would reach the channel, or a pass would lose the words the verdict re-checks"
fi
munmap_v=$(grep -nF 'v = linux_mem_munmap_verdict(' kernel/syscall_dispatch.c | head -1 | cut -d: -f1)
munmap_apply="$(sed -n "$((munmap_v - 3)),$((munmap_v + 70))p" kernel/syscall_dispatch.c)"
if [ -n "$munmap_v" ] && \
   grep -qF 'linux_group_brk(cur)' <<< "$munmap_apply" && \
   grep -qF 'linux_user_page_frame(' <<< "$munmap_apply" && \
   grep -qF 'user_unmap_page(pml4, va);' <<< "$munmap_apply" && \
   grep -qF 'user_invlpg(va);' <<< "$munmap_apply" && \
   grep -qF 'free_physical_ram_frame_for_partition' <<< "$munmap_apply" && \
   grep -qF 'v.unmap_pages' <<< "$munmap_apply"; then
    ok "X2. the reply's apply walks exactly the verdict's page range, frees every present USER frame back to the partition, shoots down each touched translation, and leaves both cursors alone — an absent page is already unmapped, not an error"
else
    bad "X2. the munmap apply is missing or no longer walks the verdict's range — a success would unmap nothing (or free a frame without invlpg / return it without unmapping)"
fi
munmap_arm=$(grep -nF '        11 => {' user/sidecar/src/linux.rs | head -1 | cut -d: -f1)
munmap_arm_body="$(sed -n "$((munmap_arm + 1)),$((munmap_arm + 40))p" user/sidecar/src/linux.rs)"
if [ -n "$munmap_arm" ] && \
   grep -qF 'checked_add(0xFFF)' <<< "$munmap_arm_body" && \
   grep -qF 'addr & 0xFFF' <<< "$munmap_arm_body" && \
   grep -qF 'addr < brk.cur' <<< "$munmap_arm_body" && \
   grep -qF 'Answer::Reply(0, STATUS_OK)' <<< "$munmap_arm_body" && \
   grep -qF 'Answer::Reply(0, E_INVAL)' <<< "$munmap_arm_body"; then
    ok "X3. the sidecar's 11 arm repeats the kernel's ladder as its own twin — nonzero non-overflowing length, aligned address, inside the span, at or above the break — and answers 0 without moving either cursor"
else
    bad "X3. the sidecar's munmap arm is gone or no longer mirrors the ladder — the kernel's first trap would be the only refusal, and a malformed range could get a fake success"
fi
if grep -qF 'linux_mem_munmap_refusal(&req, BASE, LIMIT, BASE)' tests/linux_personality_host_test.c && \
   grep -qF 'linux_mem_munmap_verdict(BASE, LIMIT, BASE,' tests/linux_personality_host_test.c && \
   grep -qF 'v.unmap_pages == 1' tests/linux_personality_host_test.c; then
    ok "X4. the host test drives the refusal and the verdict: the span/break/alignment/overflow branches, the negative-reply no-op, the non-zero malformed reply and the exact page range"
else
    bad "X4. the host test no longer drives munmap — every branch above would be reachable only at boot (t83/t117's rule)"
fi
if grep -qF 'E7MEM-MUNMAP-OK' user/sidecar/fixtures/e7_linux_probe.S && \
   grep -qF 'mov     $11, %eax' user/sidecar/fixtures/e7_linux_probe.S && \
   grep -qF 'mov     $404, %eax' user/sidecar/fixtures/e7_linux_probe.S && \
   grep -qF '.Lmunmap_bad:' user/sidecar/fixtures/e7_linux_probe.S; then
    ok "X5. the probe carries the on-target proof: two successful unmaps of the arena (the second is POSIX's already-unmapped success), the below-the-break -EINVAL, the E7MEM-MUNMAP-OK bytes out, and MARKER 404 for any wrong outcome (asserted absent)"
else
    bad "X5. the probe's munmap block is gone or gutted — the shipped boot would prove nothing about the seam while reading exactly as green"
fi
if grep -qF ' ok munmap addr=0x' user/sidecar/src/linux.rs && \
   grep -qF 'EINVAL munmap(addr=0x' user/sidecar/src/linux.rs && \
   grep -qF 'munmap addr=0x%lx len=%lu' kernel/syscall_dispatch.c; then
    ok "X6. the run's proof is on the lines: the granted range and its words, the refusal's EINVAL with the range, and the kernel's per-page free count — WHICH range and HOW MANY pages, not reconstructed from which arms ran"
else
    bad "X6. a munmap instrument line is missing — the seam would run but the log would not name the range it freed (increment 14/15's rule)"
fi

# ─── Y. E7 §10.15: the soft-protected mprotect transition ─────────────────
# The contract (Y), the first trap before any post (Y1), the reply's
# all-pages prewalk and soft apply (Y2), the page-table helper and its owner
# walk (Y3), the sidecar's 10 twin (Y4), the host tests (Y5), the probe's
# disposable fault (Y6) and the two instrument lines (Y7): each is a claim
# the transition rests on. The probe's fault is deliberate; the gate tenant
# never runs it, so the gate's no-[FAULT] clause is unchanged.
if grep -qF 'struct LinuxMprotectReq {' "$LABH" && \
   grep -qF 'int linux_mem_mprotect_refusal(const struct LinuxMprotectReq *req,' "$LABH" && \
   grep -qF 'struct LinuxMemMprotectVerdict' "$LABH" && \
   grep -qF 'int linux_mem_mprotect_refusal(const struct LinuxMprotectReq *req,' "$LABC" && \
   grep -qF 'linux_mem_mprotect_verdict(uint64_t image_lo, uint64_t image_hi,' "$LABC"; then
    ok "Y. the §10.15 transition is a named pure contract at both ends — the staged request, the first-trap refusal over the task's own image/stack/heap windows, and the reply verdict that re-runs the ladder over the same words"
else
    bad "Y. the soft-mprotect contract is missing — the kernel would have no named refusal or verdict, and the transition would be a page-table edit with no policy"
fi
mprotect_ref=$(grep -nF 'mref = linux_mem_mprotect_refusal(&mreq,' "$DISP" | head -1 | cut -d: -f1)
mprotect_first="$(sed -n "$((mprotect_ref - 3)),$((mprotect_ref + 30))p" "$DISP")"
if [ -n "$mprotect_ref" ] && \
   grep -qF 'refused before post' <<< "$mprotect_first" && \
   grep -qF 'return (uint64_t)(int64_t)mref;' <<< "$mprotect_first" && \
   grep -qF 'cur->mem_stage_a1    = largs[1];' <<< "$mprotect_first" && \
   grep -qF 'cur->mem_stage_a2    = largs[2];' <<< "$mprotect_first"; then
    ok "Y1. mprotect's first trap runs before any post: a refusal logs its words and returns its errno on the line, while a pass stages addr/len/prot for the reply's verdict"
else
    bad "Y1. the mprotect first trap is gone or no longer precedes the post — a malformed range would reach the channel, or a pass would lose the words the apply re-checks"
fi
mprotect_v=$(grep -nF 'v = linux_mem_mprotect_verdict(' "$DISP" | head -1 | cut -d: -f1)
mprotect_apply="$(sed -n "$((mprotect_v - 3)),$((mprotect_v + 80))p" "$DISP")"
if [ -n "$mprotect_v" ] && \
   grep -qF 'linux_user_page_frame(' <<< "$mprotect_apply" && \
   grep -qF 'user_protect_page(pml4, va,' <<< "$mprotect_apply" && \
   grep -qF 'user_invlpg(va);' <<< "$mprotect_apply" && \
   grep -qF 'USER_PTE_SOFT_NONE' <<< "$mprotect_apply" && \
   grep -qF 'mreq.prot == LINUX_PROT_NONE' <<< "$mprotect_apply" && \
   grep -qF 'soft-protected (P=1 U=0)' <<< "$mprotect_apply"; then
    ok "Y2. the reply prewalks every page as a present owned leaf, preserves the frame, installs P|SOFT_NONE|NX (U=0, W=0) for PROT_NONE, invlpg's each change, and names the soft state on the line — no frame is allocated, freed or zeroed"
else
    bad "Y2. the soft apply is missing or no longer walks before changing a leaf — an absent page could be half-applied, a frame could be reowned, or the log would only say 'mprotect returned 0'"
fi
if grep -qF 'int user_protect_page(uint64_t* pml4, uint64_t vaddr, uint64_t flags);' "$UPH" && \
   grep -qF 'int user_protect_page(uint64_t* pml4, uint64_t vaddr, uint64_t flags) {' "$UPC" && \
   grep -qF 'USER_PTE_SOFT_NONE' "$UPH" && \
   grep -qF 'if (!(e & (USER_PTE_USER | USER_PTE_SOFT_NONE))) return -1;' "$UPC" && \
   grep -qF 'keep = e & (USER_PTE_FRAME_MASK' "$UPC" && \
   grep -qF 'if (!(e & USER_PTE_USER) && !(e & USER_PTE_SOFT_NONE))' "$UPC"; then
    ok "Y3. the page-table helper preserves the live frame/PWT/PCD/A-D/PAT bits, refuses a supervisor-only leaf but accepts a soft one, and the teardown walk still frees a soft leaf exactly once — the frame is never leaked by PROT_NONE"
else
    bad "Y3. the soft leaf's helper or teardown exception is gone — mprotect could edit a kernel mapping, or teardown could skip the present-but-U=0 frame and leak it"
fi
mprotect_arm=$(grep -nF '        10 => {' "$LRS" | head -1 | cut -d: -f1)
mprotect_arm_body="$(sed -n "$((mprotect_arm + 1)),$((mprotect_arm + 20))p" "$LRS")"
if [ -n "$mprotect_arm" ] && \
   grep -qF 'prot & !0x7u64' <<< "$mprotect_arm_body" && \
   grep -qF 'prot & 0x2 != 0 && prot & 0x4 != 0' <<< "$mprotect_arm_body" && \
   grep -qF 'len.checked_add(0xFFF).is_none()' <<< "$mprotect_arm_body" && \
   grep -qF 'Answer::Reply(0, STATUS_OK)' <<< "$mprotect_arm_body"; then
    ok "Y4. the sidecar's 10 arm repeats the shape and W|X ladder as its own twin, grants a valid PROT_NONE without choosing an address or moving either MemBrk cursor, and leaves the page-table truth to the kernel"
else
    bad "Y4. the sidecar's mprotect arm is gone or no longer mirrors the ladder — a malformed prot could get a fake success, or the policy half would silently disappear"
fi
if grep -qF 'linux_mem_mprotect_refusal(&req' "$PHUT" && \
   grep -qF 'linux_mem_mprotect_verdict(IMG_LO' "$PHUT" && \
   grep -qF 'v.malformed == 0 && v.va == req.addr && v.pages == 2' "$PHUT" && \
   grep -qF 'user_protect_page(child, phys,' "$HUMAP" && \
   grep -qF 'teardown frees the soft leaf' "$HUMAP"; then
    ok "Y5. the host tests drive both directions: the first-trap refusals and verdict on the personality side, and the frame-preserving soft/normal transitions plus the exactly-once teardown on a fake page table"
else
    bad "Y5. a host test no longer drives the transition — every branch above would be reachable only at boot (t83/t117's rule)"
fi
if grep -qF 'E7MEM-SOFT-OK' "$FIX" && \
   grep -qF '.Lsoft:' "$FIX" && \
   grep -qF 'mov     $10, %eax' "$FIX" && \
   grep -qF 'lea     0x1000(%r12), %r13' "$FIX" && \
   grep -qF 'mov     (%r13), %rax' "$FIX" && \
   grep -qF 'mov     $405, %eax' "$FIX" && \
   grep -qF 'mov     $406, %eax' "$FIX" && \
   grep -qF 'jmp     .Lsoft' "$FIX"; then
    ok "Y6. the probe carries the disposable live case: on its success path it mprotects the second brk page PROT_NONE, writes E7MEM-SOFT-OK through the fd seam, then reads the page and must fault; MARKER 405 (refused) and 406 (read succeeded) are asserted absent"
else
    bad "Y6. the probe's soft case is gone or gutted — the boot would prove nothing about the U/S clear, and a regression would read as a quiet run"
fi
if grep -qF 'soft-protected (P=1 U=0)' "$DISP" && \
   grep -qF '[FAULT] Ring-3 #PF  error=0x%lx  addr=0x%016lx' "$STUB"; then
    ok "Y7. the live evidence has two named lines: the kernel says how many pages it soft-protected, and the Ring-3 #PF line names the faulting address — the transcript can pair them instead of inferring a fault from the task's absence"
else
    bad "Y7. one of the two live instrument lines is gone — the boot would not distinguish a real soft fault from a task that simply exited"
fi

# ─── Z. E7 TG-1 I2: the kernel-local clone (the Go thread shape) ───────────
# Clone is the first operation that can create a second descriptor in a live
# group. The source half has three independently wrong shapes: accepting a
# flag subset whose semantics the group cannot honor, synthesizing a child
# context that starts somewhere other than the parent's return RIP, and
# creating a descriptor that quietly owns a private page table or never joins
# the group. The host test drives the first two; these clauses hold the trap
# path and the process-table integration the host cannot reach.
LCHUT=tests/linux_clone_host_test.c
if grep -qE '#define LINUX_CLONE_VM[[:space:]]+0x00000100ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_FS[[:space:]]+0x00000200ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_FILES[[:space:]]+0x00000400ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_SIGHAND[[:space:]]+0x00000800ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_SYSVSEM[[:space:]]+0x00040000ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_THREAD[[:space:]]+0x00010000ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_SETTLS[[:space:]]+0x00080000ull' "$LTHH" && \
   grep -qE '#define LINUX_CLONE_CHILD_CLEARTID[[:space:]]+0x00200000ull' "$LTHH" && \
   grep -qF '#define LINUX_CLONE_GO_FLAGS (LINUX_CLONE_VM | LINUX_CLONE_FS |' "$LTHH" && \
   grep -qF '#define LINUX_CLONE_ALLOWED  (LINUX_CLONE_GO_FLAGS | LINUX_CLONE_SETTLS | \' "$LTHH" && \
   grep -qF '                              LINUX_CLONE_CHILD_CLEARTID)' "$LTHH" && \
   grep -qF 'int linux_clone_plan(' "$LTHH" && \
   grep -qF 'int linux_clone_plan(' "$LTHC" && \
   grep -qF 'flags & ~LINUX_CLONE_ALLOWED' "$LTHC" && \
   grep -qF '(flags & LINUX_CLONE_GO_FLAGS) != LINUX_CLONE_GO_FLAGS' "$LTHC" && \
   grep -qF 'child_stack == 0 || !linux_fs_addr_is_user_canonical(child_stack)' "$LTHC" && \
   grep -qF 'out->fs_base = tls;' "$LTHC" && \
   grep -qF 'out->fs_base = parent_fs_base;' "$LTHC"; then
    ok "Z1. the clone planner accepts exactly the Go shape plus its two optional promises — six mandatory shared flags, SETTLS and (since I5) CLONE_CHILD_CLEARTID, each with its own validation — refuses every outside bit with EINVAL, rejects a zero/non-canonical stack, TLS or clear-TID word, and selects the child's fs_base from TLS or the parent's live base"
else
    bad "Z1. a clone flag or the pure planner is gone — a subset would be accepted as a thread while the group still shares what the omitted flag made private, or a non-canonical child stack/TLS would reach the ring-3 context"
fi
if grep -qF 'void linux_clone_build_context(' "$LTHH" && \
   grep -qF 'void linux_clone_build_context(' "$LTHC" && \
   grep -qF 'for (int i = 0; i < 20; i++) w[i] = 0;' "$LTHC" && \
   grep -qF 'ctx->rsi   = child_stack;' "$LTHC" && \
   grep -qF 'ctx->rax   = 0;' "$LTHC" && \
   grep -qF 'ctx->rip   = parent_rip;' "$LTHC" && \
   grep -qF 'ctx->rsp   = child_stack;' "$LTHC" && \
   grep -qF 'ctx->cs    = 0x23;' "$LTHC" && \
   grep -qF 'ctx->ss    = 0x1B;' "$LTHC" && \
   grep -qF 'int proc_read_linux_return_rip(uint64_t* out)' "$PDC" && \
   grep -qF 'proc_read_linux_return_rip(&parent_rip)' "$DISP"; then
    ok "Z2. the child context is a full 20-word synthesis: every GPR cleared except rsi=child stack, rax=0, rip=the caller's captured return RIP, rsp=child stack, CS/SS=0x23/0x1B, IF on — and the dispatcher feeds it the gs:0x18 return RIP rather than a stale user_rip field"
else
    bad "Z2. the child context builder, the return-RIP capture or one of its registers is gone — the child could start at _start/a stale entry, sysret with a parent register, or iretq with a kernel selector"
fi
clone_line=$(grep -n 'if (num == LINUX_CLONE) {' "$DISP" | head -1 | cut -d: -f1)
desc_line=$(grep -n 'linux_syscall_describe(&req' "$DISP" | head -1 | cut -d: -f1)
clone_branch="$(sed -n "${clone_line},${desc_line}p" "$DISP")"
if grep -qE '#define LINUX_CLONE[[:space:]]+56u' "$LABH" && \
   grep -qF 'LSC_THREAD' "$LABH" && \
   grep -qF '{  56, LSC_THREAD' "$LABC" && \
   grep -qF 'case LSC_THREAD: return "thread";' "$LABC" && \
   [ -n "$clone_line" ] && [ -n "$desc_line" ] && \
   [ "$clone_line" -lt "$desc_line" ] && \
   grep -qF 'linux_clone_thread(cur, &cplan, parent_rip,' <<< "$clone_branch" && \
   grep -qF 'return (uint64_t)child_tid;' <<< "$clone_branch"; then
    ok "Z3. clone (56) is a real LSC_THREAD table row, and the first-trap branch runs BEFORE describe/post/park: it plans, calls linux_clone_thread, and returns the child TID directly — no sidecar request for a thread"
else
    bad "Z3. the clone row/class or its kernel-local first-trap branch is gone or moved behind the forward describe — clone would be posted to a sidecar that has no row for it, or never dispatched at all"
fi
clone_start=$(grep -n 'int linux_clone_thread(' "$PDC" | head -1 | cut -d: -f1)
clone_end=$(grep -n '^// ─── process_exit' "$PDC" | head -1 | cut -d: -f1)
clone_body="$(sed -n "${clone_start},${clone_end}p" "$PDC")"
if [ -n "$clone_start" ] && [ -n "$clone_end" ] && \
   grep -qF 'child->cr3          = g->cr3;' <<< "$clone_body" && \
   grep -qF 'child->fs_base      = plan->fs_base;' <<< "$clone_body" && \
   grep -qF 'alloc_proc_syscall_stack(parent->partition_id)' <<< "$clone_body" && \
   grep -qF 'linux_group_join_thread(g, child)' <<< "$clone_body" && \
   grep -qF 'child->has_ring3_ctx = 1;' <<< "$clone_body" && \
   grep -qF 'child->state         = PROC_SUSPENDED;' <<< "$clone_body" && \
   grep -qF 'return -LINUX_EAGAIN;' <<< "$clone_body" && \
   ! grep -qF 'user_clone_page_table' <<< "$clone_body" && \
   grep -qF 'int linux_group_join_thread(' "$LTHH" && \
   grep -qF 'int linux_group_join_thread(' "$LTHC" && \
   grep -qF 'child->group_index != LINUX_GROUP_NONE' "$LTHC" && \
   grep -qF 'child->tgid = g->group_id;' "$LTHC" && \
   grep -qF 'child->is_group_leader = 0;' "$LTHC"; then
    ok "Z4. the process-table half creates no private page table: it shares g->cr3, stamps the planned fs_base, allocates a fresh syscall stack, joins the group as a non-leader (tid/tgid/index), and only then becomes a SUSPENDED ring3 context; cap/resource exhaustion is EAGAIN"
else
    bad "Z4. the integration or group-join half is gone — the child could free/clone the shared root, keep a stale fs_base/slot state, run before it is a group member, or exceed LINUX_THREADS_MAX without EAGAIN"
fi
if grep -qF 'linux_clone_plan(GO' "$LCHUT" && \
   grep -qF 'linux_clone_plan(GO & ~required[i]' "$LCHUT" && \
   grep -qF 'linux_clone_build_context(&c, 0x0000000000401234ull, STACK)' "$LCHUT" && \
   grep -qF 'linux_group_join_thread(g, &proc_table[1])' "$LCHUT" && \
   grep -qF 'linux_group_join_thread(g, &proc_table[LINUX_THREADS_MAX])' "$LCHUT"; then
    ok "Z5. the host test drives all three contracts: the exact Go mask and every missing mandatory flag, the 20-word context with rax/rsi/rsp/rip, and group join/double-join/cap refusal"
else
    bad "Z5. the clone host test no longer exercises a branch above — the planner, context or group join would be argued from these greps alone, reachable only at boot"
fi

# ─── Z6. TG-1 I5: the thread-exit glue ─────────────────────────────────────
# Every half of the promise has one owner and one place it is spelled, and the
# live half is a marker only the JOINER can write after reading the words
# back. A mutation of the owner check, either clear, either wake, the 273 row
# or its census reddens this clause by name (tooth t160).
if grep -qF '#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ull' kernel/linux_thread.h && \
   grep -qF 'LINUX_CLONE_CHILD_CLEARTID)' kernel/linux_thread.h && \
   grep -qF 'int linux_ctid_ok(uint64_t word);' kernel/linux_thread.h && \
   grep -qF 'uint32_t linux_robust_died_word(uint32_t word, uint32_t tid);' kernel/linux_thread.h && \
   grep -qF 'int linux_robust_word_addr(uint64_t entry, int64_t futex_offset,' kernel/linux_thread.h && \
   grep -qF 'if (!linux_ctid_ok(ctid)) return -LINUX_EINVAL;' "$LTHC" && \
   grep -qF 'out->ctid = ctid;' "$LTHC" && \
   grep -qF 'if ((word & LINUX_FUTEX_TID_MASK) != (tid & LINUX_FUTEX_TID_MASK)) return 0;' "$LTHC" && \
   grep -qF 'return (word & ~LINUX_FUTEX_TID_MASK) | LINUX_FUTEX_OWNER_DIED |' "$LTHC" && \
   grep -qF 'if (back > entry) return -LINUX_EINVAL;' "$LTHC" && \
   grep -qF 'linux_thread_exit_glue(pd);' "$PDC" && \
   grep -qF 'linux_user_copy_in(zero, pd->cr3, ctid, 4)' "$PDC" && \
   grep -qF 'linux_futex_wake_threads(gid, ctid, 1);' "$PDC" && \
   grep -qF '(void)linux_futex_wake_threads(gid, waddr, 1);' "$PDC" && \
   grep -qF 'walked < LINUX_ROBUST_LIST_LIMIT' "$PDC" && \
   grep -qF 'child->linux_clear_tid = plan->ctid;' "$PDC" && \
   grep -qF 'pd->linux_clear_tid  = 0;' "$PDC" && \
   grep -qF 'if (num == LINUX_SET_ROBUST_LIST) {' "$DISP" && \
   grep -qF 'cur->linux_robust_list = largs[0];' "$DISP" && \
   grep -qF 'cur->linux_clear_tid =' "$DISP" && \
   grep -qF '{ 273, LSC_THREAD, 0 }' "$LABC" && \
   grep -qE '^273 set_robust_list [1-9][0-9]*$' tools/linux_robust_list_census.txt && \
   grep -qF 'sha256:' tools/linux_robust_list_census.txt && \
   grep -qF 'tools/linux_robust_list_census.txt' "$LABC" && \
   grep -qF 'linux_robust_died_word(T, T) ==' "$LCHUT" && \
   grep -qF 'linux_robust_list_ok(CTID, LINUX_ROBUST_LIST_HEAD_BYTES) == 1' "$LCHUT" && \
   grep -qF 'a non-canonical CLEARTID word is refused BEFORE the child exists' "$LCHUT" && \
   grep -qF 'mov     $273, %eax' user/sidecar/fixtures/e7_thread_probe.S && \
   grep -qF 'mov     $0x002D0F00, %edi' user/sidecar/fixtures/e7_thread_probe.S && \
   grep -qF 'E7TG-CTID-OK' tests/linux_thread_probe_check.sh && \
   grep -qF 'E7TG-ROBUST-OK' tests/linux_thread_probe_check.sh && \
   grep -qF 'walked 1 entries, released [1-9]' tests/linux_thread_probe_check.sh; then
    ok "Z6. the thread-exit glue is one promise with one owner per half: clone validates the CLEARTID word before the child exists and set_robust_list takes exactly the 24-byte canonical head (273's row exists because tools/linux_robust_list_census.txt measured glibc's start-up call and a program's own), the exit writes 0 through the validated copy and wakes that futex, the robust walk is bounded, checks OWNER with the mask, leaves OWNER_DIED|tid and wakes each released word, the per-thread words are re-stamped at slot reuse, and the live arm proves it from the joiner's side (E7TG-CTID-OK / E7TG-ROBUST-OK plus the kernel's clear-TID and walked-and-released lines)"
else
    bad "Z6. the exit glue lost a half: a clear that skips the validated copy is a ring-0 fault from a task's own address, a wake without a clear resumes a joiner into a tid that never went away, a robust walk without the owner check marks a live sibling's mutex dead, an unbounded walk follows a forged list, or a row without its census is a table entry built from belief"
fi

# ─── P. E7 increment 26 (P1-C): the signal frame ───────────────────────────
# Five claims, each one a way the frame could be quietly wrong: the contract
# exists and names its numbers (P), the layout is pinned by the compiler and
# the builder allocates nothing (P1), the dispatch that does NOT exist yet
# stays absent (P2), the host test actually drives every branch (P3), and no
# delivery path was smuggled in beside the builder (P4).
if grep -qF '#define LINUX_RT_SIGRETURN      15u' "$LABH" && \
   grep -qF 'struct LinuxSigFrameSpec {' "$LABH" && \
   grep -qF 'int linux_sig_frame_build(uint8_t *buf, uint32_t cap,' "$LABH" && \
   grep -qF 'int linux_sig_frame_restore(const uint8_t *buf, uint32_t len,' "$LABH" && \
   grep -qF '#define LINUX_SIGFRAME_SIZE      440u' "$LABH" && \
   grep -qF '#define LINUX_SIGFRAME_SIGMASK   304u' "$LABH" && \
   grep -qF '#define LINUX_SIGFRAME_INFO      312u' "$LABH" && \
   grep -qF 'int linux_sig_default_action(uint32_t signo);' "$LABH" && \
   grep -qF 'int linux_sig_delivery_needed(uint64_t blocked, uint32_t signo,' "$LABH" && \
   grep -qF 'int linux_sig_park_is_eintr(uint32_t signo, uint64_t blocked,' "$LABH"; then
    ok "P. the frame is a named contract: rt_sigreturn's number (15), the 440-byte rt_sigframe with its sigcontext at 48 and sigset at 304, and the three decisions — default action, delivery-needed and the EINTR rule — each declared where a host test can drive it"
else
    bad "P. the signal-frame contract is missing from kernel/linux_abi.h — the builder's numbers would live only in the .c, the EINTR rule would be an inline condition again, and no host test could pin the layout a libc reads"
fi

framesec="$(sed -n '/E7 increment 26 (P1-C/,/^int linux_sig_default_action/p' "$LABC" 2>/dev/null)"
if [ -n "$framesec" ] && \
   grep -qF 'struct LinuxSigContextMirror {' <<< "$framesec" && \
   grep -qF 'struct LinuxUContextMirror {' <<< "$framesec" && \
   grep -qF 'struct LinuxRtSigframeMirror {' <<< "$framesec" && \
   [ "$(grep -c '_Static_assert' <<< "$framesec")" -ge 10 ] && \
   grep -qF 'offsetof(struct LinuxSigContextMirror, rip) == 128' <<< "$framesec" && \
   grep -qF 'offsetof(struct LinuxSigContextMirror, cs) == LINUX_SIGCTX_CS' <<< "$framesec" && \
   grep -qF 'offsetof(struct LinuxUContextMirror, uc_sigmask) == 296' <<< "$framesec" && \
   grep -qF 'offsetof(struct LinuxRtSigframeMirror, info) == LINUX_SIGFRAME_INFO' <<< "$framesec" && \
   grep -qF 'sizeof(struct LinuxRtSigframeMirror) == LINUX_SIGFRAME_SIZE' <<< "$framesec" && \
   grep -qF 'sig_handler_stack(' <<< "$framesec" && \
   grep -qF 'rsp = ((top - (uint64_t)LINUX_SIGFRAME_SIZE) & ~(uint64_t)0xF) - 8u;' <<< "$framesec" && \
   ! grep -qE 'malloc|kmalloc|alloc_page|alloc_frame' <<< "$framesec"; then
    ok "P1. the layout is pinned by the compiler, not by prose: a mirror of sigcontext/ucontext/rt_sigframe plus ten _Static_asserts (rip at 128, cs at 144, uc_sigmask at 296, info at 312, frame 440) and Linux's own align_sigframe arithmetic — and the whole section allocates nothing, so the frame cannot leak a page on a refusal"
else
    bad "P1. the frame's layout pins are gone or the builder gained an allocator — a compiler that packed a field differently (or an edit to one offset) would ship a frame a guest libc misreads, with the failure landing in the handler instead of here"
fi

if grep -qF 'linux_syscall_class(LINUX_RT_SIGRETURN) == LSC_SIG' "$SIGFRAME_HUT" && \
   grep -qF '{  15, LSC_SIG,    0 }' "$LABC" && \
   grep -qE '^15 rt_sigreturn [1-9][0-9]*$' tools/linux_rt_sigreturn_census.txt && \
   grep -qF 'linux_syscall_class(200) == LSC_UNKNOWN' "$SYSTAB" && \
   ! grep -qF 'LINUX_RT_SIGRETURN' "$DISP"; then
    ok "P2. the row that was an asserted ABSENCE is now an asserted PRESENCE, with the same teeth: 15 is LSC_SIG in the table, the host test checks it (so deleting the row fails a test rather than passing silently), and the number's own census artifact carries the measured line — while a number nobody traced (200's stale table entry) stays UNKNOWN and the dispatcher still names neither: the row arrived by trace, and the trip is a restore, not a new dispatch"
else
    bad "P2. rt_sigreturn's classification is not backed end to end — the table row, the host test and tools/linux_rt_sigreturn_census.txt must all agree, or a row was added from belief (or deleted behind a test that kept passing): §5.6 says the table is BUILT from the trace"
fi

if grep -qF 'LINUX_SIGFRAME_SIZE == 440' "$SIGFRAME_HUT" && \
   grep -qF 'LINUX_SIGFRAME_SIGMASK == 304' "$SIGFRAME_HUT" && \
   grep -qF '(entry.r[LINUX_SIGREG_RSP] & 0xFu) == 8u' "$SIGFRAME_HUT" && \
   grep -qF 'frame[LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_CS] == 0x33' "$SIGFRAME_HUT" && \
   grep -qF 'a DISABLED altstack is not used even when SA_ONSTACK asks' "$SIGFRAME_HUT" && \
   grep -qF 'SA_NODEFER leaves the signal unblocked' "$SIGFRAME_HUT" && \
   grep -qF 'build/restore is a round trip' "$SIGFRAME_HUT" && \
   grep -qF 'SA_RESTART means the syscall is restarted instead' "$SIGFRAME_HUT" && \
   grep -qF 'a frame whose cs is not 0x33 is refused' "$SIGFRAME_HUT"; then
    ok "P3. the host test drives the frame instead of describing it: the offsets as literals, the handler's rsp%16==8 entry shape, cs==0x33 both ways (built, and refused when forged), the altstack plus its disabled/tiny fallbacks, the mask with and without SA_NODEFER, a build/restore round trip, and the EINTR/SA_RESTART split"
else
    bad "P3. the frame host test no longer exercises a branch above — the layout, the altstack choice, the mask rule or the EINTR predicate would be argued from the builder's own prose, reachable only at boot"
fi

if grep -qF 'int linux_sig_deliver(' "$LABC" && \
   grep -qF 'linux_user_copy_in(frame, root,' "$LABC" && \
   [ "$(grep -cF 'linux_user_copy_out(frame, pd->cr3,' kernel/process.c)" -eq 1 ] && \
   ! grep -qE 'setup_rt_frame|sigframe_write_user' "$LABC" "$DISP" && \
   ! grep -qF 'LINUX_RT_SIGRETURN' "$DISP"; then
    ok "P4. the frame has one validated path in EACH direction and no other: it reaches a user stack only through linux_user_copy_in against the task's own cr3, and increment 29 reads it back through exactly one linux_user_copy_out at the handler's own rsp — a direct store or load into a user page is the shape that skips every protection check, and on the return side it would be a ring-0 fault from a frame the task itself can forge"
else
    bad "P4. the frame's only writer or reader is no longer the validated copy — a direct access into a user page skips every protection check, and on the return side a frame the task can forge would be read with ring-0 privileges"
fi

if grep -qF '#define CAP_EINTR       (-21)' "$CAPH" && \
   grep -qF 'case CAP_EINTR:                  return -LINUX_EINTR;' "$LABC" && \
   grep -qF '#define LINUX_EINTR        4' "$LABH" && \
   grep -qF 'const E_INTR: i32 = -21;' "$LINUXRS" && \
   grep -qF 'fn park_interrupt(sig: &SigState) -> Option<u64> {' "$LINUXRS" && \
   grep -qF 'dispo(sig, no) == DISPO_HANDLER' "$LINUXRS" && \
   grep -qF 'sig.act[no as usize][1] & SIG_SA_RESTART == 0' "$LINUXRS" && \
   grep -qF 'fn a_parked_forward_is_woken_only_by_a_handler_without_sa_restart()' "$LINUXRS" && \
   grep -qF 'linux_sig_park_is_eintr' "$LABH" "$LABC"; then
    ok "P5. the park's wake is one rule in two languages: CAP_EINTR (-21) maps to -EINTR through §5.7 like every other code, the sidecar's park_interrupt names the same three conditions the kernel's linux_sig_park_is_eintr does (deliverable, a HANDLER, no SA_RESTART), and its six-case unit test drives each branch — so a restartable read cannot become a spurious EINTR, and a HANDLER cannot be answered with EAGAIN's code"
else
    bad "P5. the EINTR twin drifted: either the code lost its map row (leaving the sidecar's status to fall through §5.7's default and reach the task as EIO), or the sidecar's rule no longer matches the kernel's three conditions — the exact divergence a boot would show as a read that sometimes fails"
fi

if grep -qF 'r = linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES - 1u, &back);' "$SIGDELIVER_HUT" && \
   grep -qF 'r = linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES + 1u, &back);' "$SIGDELIVER_HUT" && \
   grep -qF 'if (len != LINUX_SIGDELIVER_BYTES) return -LINUX_EINVAL;' "$LABC" && \
   grep -qF '_Static_assert(sizeof(struct LinuxSigDeliverReq) == LINUX_SIGDELIVER_BYTES' "$LABH"; then
    ok "P6. the answer's shape is fixed on both sides of the wire: 80 bytes exactly, refused at 79 and 81, with the struct's size pinned to the wire length by a _Static_assert — a decoder that accepted a prefix would hand the builder a handler read out of the wrong word"
else
    bad "P6. the delivery request lost its exact-length boundary: a short or padded reply would decode into a struct with a made-up handler and restorer, and the frame would be written to an address the sidecar never named"
fi

if grep -qF 'case 11u:' "$LABC" && grep -qF 'case 7u:' "$LABC" && \
   grep -qF 'case 8u:' "$LABC" && grep -qF 'case 4u:' "$LABC" && \
   grep -qF '*out = cr2;' "$LABC" && grep -qF '*out = rip;' "$LABC" && \
   grep -qF "a NULL deref's si_addr is 0 and still counts as an answer" "$SIGDELIVER_HUT" && \
   grep -qF 'SIGFPE takes RIP' "$SIGDELIVER_HUT"; then
    ok "P7. si_addr comes from the number the signal HAS: CR2 for the two address faults (SIGSEGV, SIGBUS — including a NULL deref, where 0 is the answer and not 'no answer'), RIP for the two instruction faults (SIGFPE, SIGILL, where CR2 is stale), and the sidecar's own addr otherwise — each case driven by the host test"
else
    bad "P7. the CR2 -> si_addr rule was flattened: a crash reporter would print a stale address from whatever faulted before, or a SIGFPE would name the wrong instruction, and the guest's backtrace would point at code that never ran"
fi

if grep -qF 'CHECK(clean,' "$SIGDELIVER_HUT" && \
   grep -qF 'every byte of the guest' "$SIGDELIVER_HUT" && \
   grep -qF 'a read-only stack refuses the frame with -EFAULT' "$SIGDELIVER_HUT" && \
   grep -qF 'if (r != 0) return r;           /* nothing written: the builder refuses' "$LABC"; then
    ok "P8. a refused delivery mutates NOTHING: the builder refuses before its first store, the copy validates every page before the first byte moves, and the host test asserts the guest's stack is byte-identical by sentinel — so a signal onto an unwritable or unmapped stack is an errno, never a half-written frame in somebody's stack"
else
    bad "P8. the refusal-before-mutation property is no longer asserted: a partial frame write would be kernel-authored corruption of the task's memory, and the test that proves it did not happen is gone"
fi

if grep -qF 'LINUX_STAGE_SIGFRAME' "$LABH" "$LABC" && \
   grep -qF 'p.kind = LINUX_STAGE_SIGFRAME;' "$LABC" && \
   grep -qF 'p.len  = LINUX_SIGDELIVER_BYTES;' "$LABC" && \
   grep -qF 'p.aux  = LINUX_SIGDELIVER_BYTES;' "$LABC" && \
   grep -qF 'if (num == LINUX_TKILL)' "$LABC" && \
   grep -qF 'if (skind == LINUX_STAGE_SIGFRAME) {' kernel/chan.c && \
   grep -qF 'linux_sig_reply_apply(pd, pay, n)' kernel/chan.c && \
   grep -qF 'Answer::SignalFrame(' "$LINUXRS" && \
   grep -qF 'SIGDELIVER_BYTES' "$LINUXRS"; then
    ok "P9. the wire has ONE description of a delivery: the plan stages LINUX_STAGE_SIGFRAME for tkill with no user destination (the bytes are an instruction, not an owed copy), the shape rule demands exactly the 80-byte request, chan.c hands it to linux_sig_reply_apply instead of the pointer copy path, and the sidecar answers Answer::SignalFrame — three ends, one length, so a handler can only be reached by a message all three agree on"
else
    bad "P9. the delivery wire lost one of its three ends — a plan without the stage would refuse the sidecar's answer as payload-on-a-number-with-no-plan, a sidecar without the variant would answer E_NOSYS to a disposition that now has a frame, and a chan.c without the branch would try to copy 80 bytes to address 0"
fi

if grep -qF 'w[1]  = entry->r[LINUX_SIGREG_RIP];' "$LABC" && \
   grep -qF 'w[8]  = entry->r[LINUX_SIGREG_RSP];' "$LABC" && \
   grep -qF 'w[9]  = entry->r[LINUX_SIGREG_RDI];' "$LABC" && \
   grep -qF 'w[15] = entry->r[LINUX_SIGREG_RAX];' "$LABC" && \
   grep -qF 'sizeof(struct CapParkCtx) == 128u' kernel/process.h && \
   grep -qF 'cap_resume_linux_sig' kernel/process.h kernel/process.c && \
   [ "$(grep -A 45 'void cap_resume_linux_sig' kernel/process.c | grep -c 'swapgs')" -eq 0 ] && \
   grep -A 45 'void cap_resume_linux_sig' kernel/process.c | grep -qF 'jmp syscall_return_path' && \
   grep -qF 'the park context reads into the register file' "$SIGDELIVER_HUT"; then
    ok "P10. the handler is entered through the park context, not around it: the entry installs RIP at word 1, the frame's rsp at word 8, the signo at word 9 and (increment 29) the sysret's RAX at word 15 (the offsets cap_recv_resume's and cap_sysret_resume's asm actually read), CapParkCtx's size is asserted at 128 bytes where it is defined, and the resume is that asm's push-and-sysret WITHOUT a second swapgs — this runs inside do_syscall, where a swap would put GS_BASE back at 0 with a kernel view live"
else
    bad "P10. the install and the return path disagree — a word nudged by one is a handler entered with the syscall's rsp or the caller's rip (a #PF or a crash in a place with no relation to the signal), a missing size assert lets a field move silently, and a second swapgs in the resume corrupts GS on the way out"
fi

if grep -qF 'r = linux_sig_park_install(w, LINUX_PARK_WORDS, &entry);' kernel/process.c && \
   grep -A 2 'r = linux_sig_park_install(w, LINUX_PARK_WORDS, &entry);' kernel/process.c | grep -qF 'if (r != 0) return r;' && \
   [ "$(grep -c 'cap_resume_linux_sig(pd);' kernel/process.c)" -eq 2 ] && \
   grep -A 3 'r = linux_sig_park_install(w, LINUX_PARK_WORDS, &regs);' kernel/process.c | grep -qF 'if (r != 0) return r;' && \
   grep -qF 'pd->linux_sig_mask   = mask;' kernel/process.c && \
   grep -qF 'let before = sig.mask;' "$LINUXRS" && \
   grep -qF 'let mut frame_mask = before | sig.act[no][2] | bit;' "$LINUXRS" && \
   grep -qF 'sig.mask = frame_mask;' "$LINUXRS" && \
   grep -qF 'frame_mask &= !bit;' "$LINUXRS"; then
    ok "P11. one mask, two languages, and no resume without an install (in both directions now: the return installs through the same bridge, checks its refusal before clearing the mask, and only then resumes): the sidecar moves its mask to blocked|sa_mask|the signal (minus the signal under SA_NODEFER) BEFORE it answers, the kernel computes the same number in the frame and stores it as linux_sig_mask, and the only call into the resume sits after the install's own refusal check — so every failure path answers the syscall with an errno and the task keeps running, rather than jumping into a handler with half a context"
else
    bad "P11. the delivery's success is no longer conditional: a resume reachable from a refusal, or two masks that can disagree, is the shape where the task enters a handler the kernel did not install (or runs with a mask the sidecar did not set) and every subsequent signal behaves wrongly"
fi

if grep -qF '.Lsig_try:' "$FIX" && \
   grep -qF 'SIGHAND-OK' "$FIX" && \
   grep -qF 'mov     $200, %eax' "$FIX" && \
   grep -qF 'jmp     .Lsoft_tail' "$FIX" && \
   grep -qF '[E7] sig %u (code %u) -> handler %lx' kernel/process.c && \
   [ -s tests/linux_signal_probe_check.sh ] && \
   grep -qF 'SIGHAND-OK reached the serial log' tests/linux_signal_probe_check.sh && \
   grep -qF 'mask 200$' tests/linux_signal_probe_check.sh; then
    ok "P12. the delivery has a LIVE proof, not only a host test: the existing probe installs SIGUSR1 and raises it with tkill (13 then 200), its handler writes SIGHAND-OK and then finishes the soft-mprotect case (the probe's original terminal step, so nothing was lost by putting the delivery last), and tests/linux_signal_probe_check.sh boots the ISO and reads the kernel's own [E7] sig N -> handler line with the frame and the 0x200 mask both ends computed, with MARKERS 409/410/411 absent"
else
    bad "P12. the live proof is incomplete: a fixture without the handler or without its terminal step, a kernel that stopped printing the delivery line, or a check that no longer reads it — any of these leaves the wired delivery asserted from source only, which is exactly what this increment was not allowed to do"
fi

if [ -s tools/linux_rt_sigreturn_census.txt ] && \
   grep -qE '^15 rt_sigreturn [1-9][0-9]*$' tools/linux_rt_sigreturn_census.txt && \
   grep -qF 'sha256:' tools/linux_rt_sigreturn_census.txt && \
   grep -qF 'strace -f full trace, entry-counted' tools/linux_rt_sigreturn_census.txt && \
   grep -qF '{  15, LSC_SIG,    0 }' "$LABC" && \
   grep -qF 'tools/linux_rt_sigreturn_census.txt' "$LABC" "$SIGFRAME_HUT" && \
   grep -qF 'linux_syscall_class(LINUX_RT_SIGRETURN) == LSC_SIG' "$SIGFRAME_HUT"; then
    ok "P13. rt_sigreturn's row is CENSUSED, not believed: the artifact carries the same provenance columns as the candidate trace (method, sha256, host, date, entry-counted) and one measured row — 15, count 1, produced by a handler that returns — the table cites it by path, and the host test asserts the classification so a row could not be deleted while its test kept passing"
else
    bad "P13. the row lost its trace (or the trace lost its provenance): §5.6 says the table is BUILT from a measurement, and a row without one is the belief this artifact exists to forbid — while a deleted row with a passing test is the same lie from the other end"
fi

if grep -qF 'int linux_sig_return_apply(struct ProcessDescriptor* pd)' kernel/process.c && \
   grep -qF 'linux_user_copy_out(frame, pd->cr3, pd->park_ctx.user_rsp,' kernel/process.c && \
   grep -qF 'linux_sig_frame_restore(frame, LINUX_SIGFRAME_SIZE, &regs, &mask)' kernel/process.c && \
   grep -qF 'pd->linux_sig_mask   = mask;' kernel/process.c && \
   grep -qF 'pd->linux_sig_active = 0;' kernel/process.c && \
   grep -qF 'expect_num == LINUX_RT_SIGRETURN' kernel/chan.c && \
   grep -qF 'sig.mask = sig.saved_mask;' "$LINUXRS" && \
   grep -qF 'sig.handler_active = false;' "$LINUXRS" && \
   grep -qF 'fn rt_sigreturn_restores_the_mask_a_delivery_changed()' "$LINUXRS"; then
    ok "P14. the return trip restores BOTH halves and invents neither: the kernel reads the frame at the handler's own rsp through the validated copy (a frame the task unmapped refuses -EFAULT, never a ring-0 fault), restores the registers and uc_sigmask, clears the active flag and resumes; the sidecar puts back the mask it saved at the delivery and refuses a 15 with no handler entered — one number, two authorities, each restoring what it owns"
else
    bad "P14. the return trip lost a half: a kernel restore without the validated read is a ring-0 fault waiting for a hostile frame, a kernel that keeps its own mask while the sidecar keeps another is the divergence a second raise exposes, and a 15 accepted with no handler entered unblocks signals no delivery blocked"
fi

if grep -qF 'w[15] = entry->r[LINUX_SIGREG_RAX];' "$LABC" && \
   grep -qF 'ctx->r[LINUX_SIGREG_RAX] = w[15];' "$LABC" && \
   grep -qF 'movq 120(%1), %%rax' kernel/process.c && \
   grep -qF 'uint64_t rax;' kernel/process.h && \
   grep -qF 'w[15] == 0x7777ULL' "$SIGDELIVER_HUT" && \
   grep -qF 'restored.r[LINUX_SIGREG_RAX] == 0x7777ULL' "$SIGDELIVER_HUT"; then
    ok "P15. the sysret's RAX is part of the context: the bridge carries it in word 15, the resume loads it from 120(%1) instead of forcing zero, and the host test drives an interrupted rax of 0x7777 through build, restore, install and read-back — a return trip that dropped it would hand the caller a value the interrupted syscall never produced"
else
    bad "P15. RAX fell out of the context: a delivery wants 0 there and a return wants the interrupted value, so a hardcoded zero (or a word that is written but never read) makes every handler return look like a successful syscall"
fi

if grep -qF 'int linux_sig_return_reply_ok(uint64_t num, uint64_t rax, int32_t status);' "$LABH" && \
   grep -qF 'if (num != LINUX_RT_SIGRETURN) return 0;' "$LABC" && \
   grep -qF 'if (rax == (uint64_t)(int64_t)(-LINUX_EPROTO)) return 0;' "$LABC" && \
   grep -qF 'return status == 0;' "$LABC" && \
   grep -qF 'if (!linux_sig_return_reply_ok(expect_num, rax,' kernel/chan.c && \
   grep -A 2 'if (!linux_sig_return_reply_ok(expect_num, rax,' kernel/chan.c | grep -qF 'return rax;' && \
   ! grep -qF 'rax != (uint64_t)(int64_t)(-LINUX_EPROTO)) {' kernel/chan.c && \
   grep -qF 'linux_sig_return_reply_ok(LINUX_RT_SIGRETURN, 0, 0) == 1' "$SIGDELIVER_HUT" && \
   grep -qF 'linux_sig_return_reply_ok(LINUX_RT_SIGPROCMASK, 0, 0) == 0' "$SIGDELIVER_HUT"; then
    ok "P16. the return trip's gate is the sidecar's answer, named once: status 0 from its 15 arm (the only condition under which it put saved_mask back) opens the restore, a nonzero status — its refusal, no handler entered — falls through to the errno the §5.7 map produced with the frame untouched, a reply that never applied is refused by its -EPROTO rather than by describe's stale zero, and no number but 15 reaches it; the old \"not -EPROTO\" shortcut that restored under an E_INVAL is gone"
else
    bad "P16. the gate drifted back to a non-answer: a restore reachable on any reply that is merely not -EPROTO lets a 15 the sidecar refused (no handler entered) resume the task into a context whose mask was never unwound, with the errno silently swallowed — the divergence a second raise exposes"
fi

# The refusal branch of the gate, proven on a boot rather than only by the
# host test and t158: the probe issues 15 BEFORE it installs SIGUSR1 (the
# order is the "no handler entered"), expects exactly -EINVAL, marks it and
# issues 412 on anything else; the check reads that marker from the serial
# log and insists the boot holds exactly ONE restore line — the handler's.
sigret_sec="$(sed -n '/^\.Lsig_try:/,/^\.Lsig_install_bad:/p' "$FIX" 2>/dev/null)"
sr15="$(grep -n 'mov     \$15, %eax' <<< "$sigret_sec" | head -1 | cut -d: -f1)"
sr13="$(grep -n 'mov     \$13, %eax' <<< "$sigret_sec" | head -1 | cut -d: -f1)"
if [ -n "$sigret_sec" ] && [ -n "$sr15" ] && [ -n "$sr13" ] && [ "$sr15" -lt "$sr13" ] && \
   grep -qF 'cmp     $-22, %rax' <<< "$sigret_sec" && \
   grep -qF 'mov     $412, %eax' <<< "$sigret_sec" && \
   grep -qF 'SIGRET-NOHAND-OK' <<< "$sigret_sec" && \
   grep -qF 'SIGRET-NOHAND-OK' tests/linux_signal_probe_check.sh && \
   grep -qF 'exactly ONE [E7] rt_sigreturn: restored line' tests/linux_signal_probe_check.sh && \
   grep -qF "412) why='a 15 with no handler entered was not -EINVAL'" tests/linux_signal_probe_check.sh; then
    ok "P17. the refusal is LIVE, not only a host test and a tooth: the probe issues rt_sigreturn(15) before it installs SIGUSR1 — so no handler has been entered by construction — and requires exactly -EINVAL, marking it SIGRET-NOHAND-OK and issuing 412 on any other answer; tests/linux_signal_probe_check.sh reads that marker from the serial log, requires exactly ONE restore line in the whole boot (the handler's own return) so a refused 15 that restored anyway cannot hide, orders the marker before that line, and keeps 412 absent"
else
    bad "P17. the refusal branch lost its live proof: without a 15 issued before the install the boot never exercises the gate's else-arm, and without the one-restore count a refused 15 that restored a frame nobody built would look exactly like a passing boot — the branch would be back to being asserted from a host test alone"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "linux_personality_check: all clauses hold"
    exit 0
fi
echo "linux_personality_check: $fails clause(s) failed"
exit 1

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
    arch/x86/user_paging.c
    arch/x86/syscall.asm
    user/sidecar/src/linux.rs
    user/sidecar/src/entry.rs
    user/sidecar/src/e7_probe.rs
    user/sidecar/src/e7_candidate.rs
    user/sidecar/src/e7_firstuser.rs
    user/proto/src/manifest.rs
    user/sidecar/fixtures/e7_linux_probe.S
    user/sidecar/fixtures/build_candidate.sh
    kernel/elf64_load.h
    kernel/elf64_load.c
    # O8 reads the host test that EXECUTES the mid-page case (E8a), so the
    # seeded copy needs it exactly as the guard does.
    tests/cap_create_sidecar_host_test.c
    # Q (the SSE gate) reads the enable, its call site, the vector-7 chain
    # and the host test that pins the emitted order — same rule as above.
    arch/x86/fpu_enable.h
    arch/x86/lazy_fpu.c
    arch/x86/idt.c
    arch/x86/interrupt.asm
    arch/x86/switch_lazy.asm
    kernel/kernel.c
    kernel/stubs.c
    tests/fpu_enable_host_test.c
    # R (the signal seam) reads the plan's host test, same rule.
    tests/linux_personality_host_test.c
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
echo "and increment-3 delivery guard, §5.3's one-MSR seam (clause set L),"
echo "increment 5's fd-class byte crossing (clause set M), increment 6's"
echo "mem-class budget span (clause set N), and increment 7's image class —"
echo "the ONE ELF mapper, the §5.2 stack and its environment-crossing"
echo "record (clause set O, including O6/O7)"
echo "and increment 10's signal seam (clause set R) — the three-number"
echo "answers, the plan's first-trap refusals and the SignalKill delivery"
echo "and increment 11's poll answer (clause set S) — the run's LAST"
echo "ENOSYS: a zero-timeout probe answered 0, a waiting poll refused"
echo "at both ends before any post"
echo "and increment 12's reply payload (clause set T) — the copy-IN's"
echo "first-trap destination plan, the deliver's three shapes, the"
echo "two-phase writable copy and the stage's four-place lifecycle"
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
sed -i '/r = cap_send_msg(pid, ch_w, msg, msg_len, NULL, 0, 0, 0);/d' "$W/t9/kernel/chan.c"
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
sed -i 's/crate::linux::service(\&RealKernel, \&booted\.console);//' "$W/t14/user/sidecar/src/entry.rs"
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

# ─── §5.3 teeth (the plan's increment 4: the one-MSR seam) ──────────────

# L: the writer's body gutted — a user_write_fs_base that writes nothing is
# the worst form of this bug: every caller looks correct, the MSR never moves.
seed "$W/t18"
sed -i 's/    wrmsr(MSR_FS_BASE, base);//' "$W/t18/arch/x86/user_paging.c"
tooth fs-writer-gutted L "user_write_fs_base no longer writes MSR_FS_BASE" "$W/t18"

# L1: the gate's zeroing dropped — the invariant then starts FALSE and every
# later switch-in merely happens to overwrite firmware's leftover value.
seed "$W/t19"
sed -i 's/    wrmsr(MSR_FS_BASE, 0);//' "$W/t19/arch/x86/user_paging.c"
tooth fs-baseline-gone L1 "gate init no longer zeroes MSR_FS_BASE" "$W/t19"

# L2: the slot-reuse clear deleted — the reused slot hands the next occupant
# its predecessor's TLS base, live in the MSR, from its very first schedule.
seed "$W/t20"
sed -i '/pd->fs_base = 0;/d' "$W/t20/kernel/process.c"
tooth fs-clear-dropped L2 "the reused-slot fs_base reset is gone" "$W/t20"

# L3: the switch-in write deleted — two Linux tasks running back to back
# silently share the first one's TLS base, with nothing observable at all.
seed "$W/t21"
sed -i '/user_write_fs_base(next->fs_base);/d' "$W/t21/kernel/process.c"
tooth fs-switch-unloaded L3 "the switch-in no longer loads the incoming task's FSBASE" "$W/t21"

# L4: the apply write gutted — the grant is decided, the reply accepted, and
# the task returns to a base that never moved: a silent lie to ring-3.
seed "$W/t22"
sed -i 's/ *user_write_fs_base(cur->fs_base);//' "$W/t22/kernel/syscall_dispatch.c"
tooth fs-apply-missing L4 "the granted SET_FS no longer writes the MSR at the reply" "$W/t22"

# L5: the predicate's cmd word dropped — now ANY arch_prctl (GET_FS, SET_GS,
# an unknown cmd) stages a write of args[1], and the apply turns a GET into
# a SET the sidecar never granted.
seed "$W/t23"
sed -i 's/return num == LINUX_ARCH_PRCTL && cmd == LINUX_ARCH_SET_FS;/return num == LINUX_ARCH_PRCTL;/' "$W/t23/kernel/linux_abi.c"
tooth fs-predicate-loosened L5 "the stage/apply predicate ignores the cmd word" "$W/t23"

# L6: the sidecar's canonical bound widened to "any u64" — the grant then
# waves through exactly the values the kernel's re-check exists to refuse,
# and -EINVAL becomes unreachable from the policy side.
seed "$W/t24"
sed -i 's/(0x1002, a) if a <= 0x0000_7FFF_FFFF_FFFF => {/(0x1002, a) if a <= 0xFFFF_FFFF_FFFF_FFFF => {/' "$W/t24/user/sidecar/src/linux.rs"
tooth fs-sidecar-bound-loosened L6 "the sidecar's FS-base bound admits every u64" "$W/t24"

# ─── Increment-5 teeth (the fd class: the kernel copy, the fd table) ──────

# M1: the chunk grown past the channel's payload bound — the crossing dies
# at post time for big writes only, while every small-write clause stays green.
seed "$W/t25"
sed -i 's/#define LINUX_FD_WRITE_CHUNK  *[0-9]*u/#define LINUX_FD_WRITE_CHUNK  8192u/' "$W/t25/kernel/linux_abi.h"
tooth fd-chunk-oversized M1 "the write chunk exceeds the channel payload bound" "$W/t25"

# M2: the copy never attached — the header then describes bytes that are
# not in the message, and the sidecar's invariant check (rightly) refuses
# every write as malformed.
seed "$W/t26"
sed -i '/linux_user_copy_out(fwd +/d' "$W/t26/kernel/syscall_dispatch.c"
tooth fd-copy-detached M2 "the copy call is gone — no bytes ever attach" "$W/t26"

# M2: the clamp dropped from the description — header, payload and the rax
# the task will read become three different truths.
seed "$W/t27"
sed -i 's/if (plan.len) largs\[2\] = plan.len;//' "$W/t27/kernel/syscall_dispatch.c"
tooth fd-clamp-dropped M2 "args[2] no longer carries the clamp" "$W/t27"

# M3: the send hard-codes 72 — the attached bytes exist everywhere in the
# tree except the one call that crosses the channel.
seed "$W/t28"
sed -i 's/r = cap_send_msg(pid, ch_w, msg, msg_len, NULL, 0, 0, 0);/r = cap_send_msg(pid, ch_w, msg, 72, NULL, 0, 0, 0);/' "$W/t28/kernel/chan.c"
tooth fd-send-drops-bytes M3 "the post sends only the header, dropping the payload" "$W/t28"

# M4: the USER check deleted — the copy then reads any present page the
# walk reaches, which is the whole security boundary of the crossing.
seed "$W/t29"
sed -i '/!(e & LAB_PTE_USER)/d' "$W/t29/kernel/linux_abi.c"
tooth fd-copy-user-check-gone M4 "the walker no longer requires USER at every level" "$W/t29"

# M5: the EBADF constant drifted — every unknown-descriptor refusal would
# reach the task as a different errno than the policy meant (§5.7).
seed "$W/t30"
sed -i 's/const E_BADF: i32 = -2;/const E_BADF: i32 = -1;/' "$W/t30/user/sidecar/src/linux.rs"
tooth fd-ebadf-drifted M5 "the sidecar's E_BADF no longer pins CAP_EBADF (-2)" "$W/t30"

# M6: the payload invariant neutralised — bytes that are not the ones the
# task wrote (or a count that disagrees with the header) would be performed
# on the console without a check.
seed "$W/t31"
sed -i 's/if rr.len != expected {/if false {/' "$W/t31/user/sidecar/src/linux.rs"
tooth fd-payload-check-off M6 "the request-length invariant is dead-coded" "$W/t31"

# ─── Increment-6 teeth (the mem class: the budget span, the W^X refusal) ──

# N1: the slot-reuse clear loses the break — a reused slot's brk_cur is
# its predecessor's, and the next occupant's first granted growth maps
# pages at a break nobody asked for.
seed "$W/t32"
sed -i '/pd->linux_brk_cur = 0;/d' "$W/t32/kernel/process.c"
tooth mem-clear-dropped N1 "the reused-slot brk state reset is gone" "$W/t32"

# N2: the stamp made unconditional — every SLS sidecar (init, DM, every
# driver) is handed a Linux heap span, and the span no longer follows the
# manifest that declared it.
seed "$W/t33"
python3 - "$W/t33/kernel/cap.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = "    if (personality == PROC_PERSONALITY_LINUX) {\n        pd->linux_heap_base"
new = "    if (1) {   /* SLS slots get a Linux span too */\n        pd->linux_heap_base"
assert old in s, "anchor not found"
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
tooth mem-span-unconditional N2 "the span stamp is no longer keyed on the LINUX personality" "$W/t33"

# N3: the kernel's first-trap W|X refusal dead-coded — now only the
# sidecar's arm refuses, and one Rust edit away from W|X pages.
seed "$W/t34"
sed -i 's/linux_mprotect_refuses_wx(largs\[2\])) {/0) {/' "$W/t34/kernel/syscall_dispatch.c"
tooth wx-firsttrap-off N3 "the dispatcher's first-trap W|X refusal is dead-coded" "$W/t34"

# N3: the sidecar's twin dead-coded — now only the kernel's plan refuses.
seed "$W/t35"
sed -i 's/if args\[2\] & 0x2 != 0 && args\[2\] & 0x4 != 0 {/if false {/' "$W/t35/user/sidecar/src/linux.rs"
tooth wx-sidecar-off N3 "the sidecar's W|X arm is dead-coded" "$W/t35"

# N4: the verdict's malformed branch dead-coded — a grant that escaped the
# span or echoed the wrong value would still be mapped.
seed "$W/t36"
sed -i 's/if (v.malformed) {/if (0) {/' "$W/t36/kernel/syscall_dispatch.c"
tooth brk-malformed-off N4 "the verdict's malformed branch is dead-coded" "$W/t36"

# N5: the un-partitioned allocator swapped in — brk pages bypass the
# frame quota entirely, the very ceiling §5.4 rests on.
seed "$W/t37"
sed -i 's/void \*frame = allocate_physical_ram_frame_for_partition(cur->partition_id);/void *frame = allocate_physical_ram_frame();/' "$W/t37/kernel/syscall_dispatch.c"
tooth mem-uncharged-frames N5 "brk frames no longer come from the partition-charged allocator" "$W/t37"

# N5: the zero-fill dropped — the pool recycles frames across tasks and
# partitions, so a fresh heap page would carry its last tenant's bytes.
seed "$W/t38"
sed -i '/w\[zi\] = 0;/d' "$W/t38/kernel/syscall_dispatch.c"
tooth mem-unzeroed-frames N5 "brk frames are no longer zero-filled before the task can read them" "$W/t38"

# N6: the unwind's read-back deleted — a frame-exhausted grant leaks every
# frame it already mapped, and nobody can say which they were.
seed "$W/t39"
sed -i '/f = linux_user_page_frame(/d' "$W/t39/kernel/syscall_dispatch.c"
tooth mem-unwind-gone N6 "the mid-batch unwind no longer reads the frames back" "$W/t39"

# N7: the refusal shape lies — an ENOSYS where Linux returns the CURRENT
# break: every libc's brk wrapper reads the return against its request.
seed "$W/t40"
sed -i 's/Answer::Reply(brk.cur, STATUS_OK)/Answer::Reply(0, E_NOSYS)/' "$W/t40/user/sidecar/src/linux.rs"
tooth brk-refusal-errno N7 "the brk refusal answers an errno instead of the unchanged break" "$W/t40"

# N8: the probe's positive marker deleted — the mem block would still run
# but prove nothing on serial, which is exactly the vacuity the marker
# discipline exists to refuse.
seed "$W/t41"
sed -i '/E7MEM-BRK-OK/d' "$W/t41/user/sidecar/fixtures/e7_linux_probe.S"
tooth mem-probe-marker-gone N8 "the probe's mem-block positive marker is gone" "$W/t41"

# ─── Increment-7 teeth (the image class: the ONE ELF mapper, the §5.2 stack) ──

# O1: the ET_EXEC gate inverted — every real candidate IS ET_EXEC, so the
# inverted gate refuses the only images the mapper exists for and admits
# the ET_DYN it must never bias (§5.1).
seed "$W/t42"
sed -i 's/if (ehdr->e_type != ELF_ET_EXEC) {/if (ehdr->e_type == ELF_ET_EXEC) {/' "$W/t42/kernel/elf64_load.c"
tooth elf-gate-inverted O1 "the ET_EXEC gate is inverted (ET_EXEC refused, everything else admitted)" "$W/t42"

# O2: the magic dispatch deleted — every ELF falls into the flat mapper
# and enters at base+offset while its segments sit at their linked vaddrs.
seed "$W/t43"
sed -i '/image_is_elf = elf_blob_has_magic(/d' "$W/t43/kernel/cap.c"
tooth elf-dispatch-gone O2 "the create's magic dispatch is gone" "$W/t43"

# O3a: region_vaddr pointed at the HOST/phys address of the stack region —
# every pointer in the image and the returned %rsp become physical
# addresses, which a libc dereferences in its OWN space and faults on.
# (The live bug increment 7's measurement caught; the cap host test pins
# the fixed form.)
seed "$W/t44"
sed -i 's/sp.region_vaddr = stack_base;/sp.region_vaddr = linux_stack_phys;/' "$W/t44/kernel/cap.c"
tooth stack-image-phys-pointers O3 "the stack image's pointers are no longer child vaddrs" "$W/t44"

# O3b: the image's region grown back over the BIB's top page — step 9's
# unconditional BIB write then clobbers the strings/auxv the builder just
# wrote, and the child reads its own argc as the BIB magic.
seed "$W/t45"
sed -i 's/(size_t)stk_pages \* 4096 - 4096, &sp)/(size_t)stk_pages * 4096, \&sp)/' "$W/t45/kernel/cap.c"
tooth stack-image-bib-clobber O3 "the §5.2 image no longer stops below the BIB page" "$W/t45"

# O4: the entropy gate dead-coded — a zero-filled AT_RANDOM ships silently
# and every canary seeded from it is worth nothing.
seed "$W/t46"
sed -i 's/if (entropy_get(rnd, sizeof rnd) != ENTROPY_OK) {/if (0) {/' "$W/t46/kernel/cap.c"
tooth stack-image-no-entropy O4 "the AT_RANDOM entropy gate is dead-coded" "$W/t46"

# O5: the candidate's spawn arm deleted — the boot measures only the flat
# mapper again and every §5.1/§5.2 clause stays argued, not observed.
seed "$W/t47"
sed -i '/crate::e7_candidate::spawn(/d' "$W/t47/user/sidecar/src/entry.rs"
tooth candidate-spawn-gone O5 "the census candidate's spawn arm is gone" "$W/t47"

# O1b: the mapper routed back through user_map_page — a low-linked
# segment's leaf is written through the shared kernel identity map's
# huge-page entry and the child's first instruction fetch #PFs (the live
# finding of increment 7's first census boot: rip=0x400000, error 0x15,
# [LOADER] claiming R-X).
seed "$W/t48"
sed -i 's/cap_arch_map_page((uint64_t)(uintptr_t)pml4,/user_map_page(pml4,/' "$W/t48/kernel/elf64_load.c"
tooth elf-unsafe-walk O1 "the ELF mapper routes its segments through user_map_page (the unsafe walk)" "$W/t48"

# ─── O6/O7 teeth (§5.2's environment-crossing record, ARGENV) ───────────────

# t49 (O6): the site's envp consult deleted — the record parses and is
# then IGNORED, and every Linux child silently gets the empty environment
# again while argc (the only thing the other clauses read) looks right.
seed "$W/t49"
sed -i '/for (uint32_t i = 0; i < m.envc; i++)/d' "$W/t49/kernel/cap.c"
sed -i '/envp\[m.envc\] = 0;/d' "$W/t49/kernel/cap.c"
tooth envp-consult-gone O6 "the §5.2 site no longer consults the ARGENV record's envp" "$W/t49"

# t50 (O6): the parse's exact-consumption refusal neutralised — a record
# with bytes trailing its strings would be silently accepted (and, with
# the other refusals edited the same way, truncated lists would read as
# success — the failure mode refusing-by-name exists to end).
seed "$W/t50"
sed -i 's/if (q != rp + rlen) return CAP_EINVAL;/if (0) {}/' "$W/t50/kernel/cap.c"
tooth argenv-parse-loosened O6 "the ARGENV parse no longer refuses trailing bytes" "$W/t50"

# t51 (O7): the firstuser's record dropped — the designated first user
# (the only child that READS its environment) would cross with none, and
# the boot would read green because argc is still { name } either way.
seed "$W/t51"
sed -i 's/argenv: Some(ArgEnv::new(\&\[USER_NAME\], LINUX_DEFAULT_ENV)),/argenv: None,/' "$W/t51/user/sidecar/src/e7_firstuser.rs"
tooth firstuser-argenv-gone O7 "the firstuser no longer crosses the ARGENV record" "$W/t51"

# t52 (O7): the shared default-environment list renamed out from under
# the spawn sites — the two halves of the twin would drift apart, which
# is why the list is ONE constant rather than three inline copies.
seed "$W/t52"
sed -i 's/pub const LINUX_DEFAULT_ENV/pub const XENV/' "$W/t52/user/sidecar/src/linux.rs"
tooth env-list-gone O7 "the shared LINUX_DEFAULT_ENV list is gone from linux.rs" "$W/t52"

# ─── O8 teeth (increment 8, step 1: a PT_LOAD that starts MID-PAGE) ────────

# t53: the page anchor pointed back at the SEGMENT's start — the exact
# assumption that killed the first user (§10.13's step-0 diagnosis): every
# byte of a mid-page segment lands (p_vaddr & 0xFFF) early in vaddr space,
# .init_array reads back 0x28, and the child dies at rip=0x28 before main.
seed "$W/t53"
sed -i 's/uint64_t page_base = s->p_vaddr & ~(uint64_t)4095u;/uint64_t page_base = s->p_vaddr;/' "$W/t53/kernel/elf64_load.c"
tooth elf-midpage-shift O8 "the mapper anchors at the SEGMENT start again, so a mid-page LOAD lands (p_vaddr \& 0xFFF) bytes early" "$W/t53"

# t54: the shared-page refusal dead-coded — a second LOAD covering a page
# the first already owns maps the one frame twice, silently discarding the
# first segment's bytes and its permissions.
seed "$W/t54"
sed -i 's/if (a_lo < b_hi && b_lo < a_hi) {/if (0) {}/' "$W/t54/kernel/elf64_load.c"
tooth elf-shared-page-no-refusal O8 "two LOADs sharing a page are no longer refused by name" "$W/t54"

# ─── Q teeth (the SSE gate: CR4.OSFXSR, its order, and the vector-7 chain) ─

# t70: the BSP call deleted — the gate exists, is compiled, is host-tested,
# and runs on NOBODY: every ring-3 SSE instruction is #UD again (the
# measured musl-abort pxor death) while every other clause stays green.
seed "$W/t70"
sed -i '/^    fpu_enable_sse();$/d' "$W/t70/kernel/kernel.c"
tooth sse-gate-never-called Q1 "kernel.c no longer calls fpu_enable_sse() before the trap gate opens ring 3" "$W/t70"

# t71: the OSFXSR bit moved one position — CR4 gains bit 8 (reserved!) and
# never gains bit 9, so SSE stays #UD; a wrong bit is exactly the class of
# error a comment cannot catch, which is why the host test pins the SDM's
# own numbers and this clause pins the name.
seed "$W/t71"
sed -i '0,/(1ull << 9)/s//(1ull << 8)/' "$W/t71/arch/x86/fpu_enable.h"
tooth sse-osfxsr-bit-moved Q1 "CR4.OSFXSR is no longer bit 9" "$W/t71"

# t72: the CPUID refusal neutralised — the CR4 write now happens on any CPU
# regardless of SSE support; on one without SSE the write #GPs and the
# machine triple-faults under -no-reboot with no trace at all.
seed "$W/t72"
sed -i 's/if (!fpu_cpuid_has_sse(edx)) {/if (0) {}/' "$W/t72/arch/x86/lazy_fpu.c"
tooth sse-cpuid-refusal-gone Q2 "the SSE gate no longer refuses by name on a CPU without SSE" "$W/t72"

# t73: the ldmxcsr deleted — the x87 core is initialised but MXCSR keeps
# whatever garbage it powers up with, so the "all exceptions masked"
# invariant the runtime assumes is not actually established.
seed "$W/t73"
sed -i '/__asm__ volatile("ldmxcsr/d' "$W/t73/arch/x86/lazy_fpu.c"
tooth sse-mxcsr-unloaded Q2 "the gate no longer loads the masked-reset MXCSR after fninit" "$W/t73"

# t74: the vector-7 gate pointed at the wrong stub — a #NM (reachable the
# moment anyone arms CR0.TS) lands on a stub that does not handle it: the
# same silent-triple-fault class under -no-reboot that made the #UD
# diagnosis possible only by luck of the general handler existing.
seed "$W/t74"
sed -i 's/(uint64_t)isr7_stub/(uint64_t)isr9_stub/' "$W/t74/arch/x86/idt.c"
tooth sse-nm-gate-misrouted Q3 "vector 7 no longer routes to isr7_stub" "$W/t74"

# t75: a call site for the dormant lazy switch appears — CR0.TS would now
# arm on every switch-in while the handler's xsave needs CR4.OSXSAVE,
# which the gate deliberately never sets: the first #NM would #UD inside
# its own handler, and the doc's 'XMM is not switched per task' claim
# stops being true without anything reddening.
seed "$W/t75"
printf '\nvoid sse_smoke_t75(void);
void sse_smoke_t75(void) { extern void perform_lazy_context_switch(void*, void*); perform_lazy_context_switch(0, 0); }
' >> "$W/t75/kernel/process.c"
tooth sse-lazy-woken Q4 "a .c file now calls perform_lazy_context_switch() (the lazy path is no longer dormant)" "$W/t75"

# ─── R: the signal seam (§10.13's step 2) ──────────────────────────────

# t76: the sigsetsize refusal silenced — a caller claiming a 4-byte sigset
# would have its bytes silently reinterpreted as an 8-byte mask, and the
# host test's literal -EINVAL check would catch it too, but the GUARD must
# be the thing that reddens at source level.
seed "$W/t76"
sed -i 's/p.refuse = -LINUX_EINVAL;/p.refuse = 0;/' "$W/t76/kernel/linux_abi.c"
tooth sigplan-einval-refusal-gone R1 "the plan's sigsetsize refusal no longer refuses" "$W/t76"

# t77: the dispatch's first-trap return deleted — the refusal is still
# computed, but the request now posts anyway: the sidecar answers a call
# whose kernel half already decided it could not be served.
seed "$W/t77"
sed -i '/return (uint64_t)(int64_t)splan.refuse;/d' "$W/t77/kernel/syscall_dispatch.c"
tooth sigplan-refusal-returns-late R2 "the dispatch no longer returns the plan's refusal from the first trap" "$W/t77"

# t78: the tkill arm renamed away — the number the firstuser's raise()
# takes (tkill, 200) falls to the default ENOSYS arm, so SIGABRT never
# terminates and abort() walks into its hlt: the exact boot death this
# seam exists to end, back by name.
seed "$W/t78"
sed -i 's/LINUX_TKILL => {/LINUX_TKILL_NEVER => {/' "$W/t78/user/sidecar/src/linux.rs"
tooth tkill-arm-gone R4 "the sidecar no longer answers tkill (200)" "$W/t78"

# t79: the terminate arm answers success — the silent lie of this whole
# clause set: raise(SIGABRT) returns 0, abort() believes the signal was
# delivered, and the task keeps running instead of being reaped.
seed "$W/t79"
sed -i 's/ACT_TERMINATE => Answer::SignalKill(args\[1\]),/ACT_TERMINATE => Answer::Reply(0, STATUS_OK),/' "$W/t79/user/sidecar/src/linux.rs"
tooth terminate-answers-success R4 "a default-terminate signal no longer converges on SignalKill" "$W/t79"

# t80: the handler-install refusal answered 0 — v1 has no signal frames,
# so a recorded handler would never run while its caller believes the
# install succeeded.
seed "$W/t80"
sed -i 's|_ => Answer::Reply(0, E_NOSYS), /\* handler install not landed \*/|_ => Answer::Reply(0, STATUS_OK), /* handler install silently kept */|' "$W/t80/user/sidecar/src/linux.rs"
tooth handler-install-answered-0 R5 "a handler install is no longer refused by name before any mutation" "$W/t80"

# t81: tkill's tid scope deleted — any sender may signal any tid through
# this arm, silently re-targeted instead of answered ESRCH.
seed "$W/t81"
sed -i 's/if sig.tid == 0 || args\[0\] as u32 != sig.tid as u32 {/if false {/' "$W/t81/user/sidecar/src/linux.rs"
tooth tkill-tid-scope-gone R5 "tkill no longer requires the tid to be this task's own set_tid_address echo" "$W/t81"

# t82: rt_sigprocmask out of the class table — its payload never crosses
# (the dispatch's plan branch keys off LSC_SIG) while the census row still
# shows the number as served.
seed "$W/t82"
sed -i 's/{  14, LSC_SIG,/{  14, LSC_MEM,/' "$W/t82/kernel/linux_abi.c"
tooth sigprocmask-class-dropped R3 "rt_sigprocmask is no longer classed LSC_SIG" "$W/t82"

# t83: the plan's host-test section deleted — the pure half would then be
# argued from the guard's greps alone, every branch reachable only at boot.
seed "$W/t83"
sed -i '/linux_sig_copy_plan(/d' "$W/t83/tests/linux_personality_host_test.c"
tooth sigplan-unhosttested R6 "the host test no longer drives linux_sig_copy_plan" "$W/t83"

# ─── S teeth (increment 11: poll (7), the run's last ENOSYS answer) ────────

# t84: the predicate neutered to "refuse every poll" — a plausible
# "simplification" that puts the firstuser's wall straight back (the
# zero-timeout probe refuses again) while the dispatcher's wiring and the
# sidecar's arm both stay exactly as they were.
seed "$W/t84"
sed -i 's/return num == LINUX_POLL && (int32_t)timeout != 0;/return num == LINUX_POLL;/' "$W/t84/kernel/linux_abi.c"
tooth poll-predicate-neutered S "linux_poll_refuses_blocking no longer distinguishes the probe from the wait" "$W/t84"

# t85: the first-trap refusal block deleted — the waiting poll now posts
# and parks a task whose wake cannot come (no console-input state), and
# the sidecar's twin would be the only wall left, answering AFTER the
# park instead of the first trap refusing before any channel.
seed "$W/t85"
sed -i '/else if (linux_poll_refuses_blocking(num, largs\[2\])) {/,/(-LINUX_ENOSYS);/d' "$W/t85/kernel/syscall_dispatch.c"
tooth poll-refusal-not-first-trap S1 "the dispatcher no longer refuses a waiting poll before any post" "$W/t85"

# t86: the sidecar arm renamed away — the zero-timeout probe falls to the
# default ENOSYS arm, so the firstuser's measured wall (num=00000007)
# returns by name while the kernel's first-trap half stays green.
seed "$W/t86"
sed -i 's/^        7 => {$/        700 => {/' "$W/t86/user/sidecar/src/linux.rs"
tooth poll-arm-gone S2 "the sidecar no longer answers poll (7)" "$W/t86"

# t87: the arm's condition inverted — the probe now refuses ENOSYS (the
# wall back) and the waiting poll is granted 0, both lies in one edit.
seed "$W/t87"
sed -i 's/if (args\[2\] as i32) == 0 {/if (args[2] as i32) != 0 {/' "$W/t87/user/sidecar/src/linux.rs"
tooth poll-answers-inverted S2 "the probe and the wait answer each other's results" "$W/t87"

# t88: the arm made unconditional — every poll, including a five-second
# wait, is answered 0 events into the caller's own loop (the silent lie
# the by-name refusal exists to prevent).
seed "$W/t88"
sed -i 's/if (args\[2\] as i32) == 0 {/if true {/' "$W/t88/user/sidecar/src/linux.rs"
tooth poll-wait-granted S2 "a waiting poll is granted 0 events instead of refused by name" "$W/t88"

# t89: the predicate's host-test section deleted — the pure half would
# then be argued from the guard's greps alone, the int-width rule and
# the sign-extension case reachable only at boot (t83's rule, one seam
# along).
seed "$W/t89"
sed -i '/linux_poll_refuses_blocking(/d' "$W/t89/tests/linux_personality_host_test.c"
tooth poll-unhosttested S3 "the host test no longer drives linux_poll_refuses_blocking" "$W/t89"

# ─── T: the reply payload's copy-IN (increment 12, §10.13's step 3) ────────

# t90: the reply plan's first-trap refusal deleted — a destination ring 3
# could not write would post anyway, and the deliver's copy would land
# bytes at a span nothing validated (T1's own refusal-by-name rule).
seed "$W/t90"
sed -i '/return (uint64_t)(int64_t)rp.refuse;/d' "$W/t90/kernel/syscall_dispatch.c"
tooth reply-plan-refusal-gone T1 "the dispatch computes the destination plan but can no longer refuse it" "$W/t90"

# t91: the stage never set — every reply payload then arrives for a
# number with "no reply plan" and is refused, so the out-pointer the
# caller passed keeps its stack garbage (the silent lie, by refusal).
seed "$W/t91"
sed -i '/cur->reply_stage_valid = 1;/d' "$W/t91/kernel/syscall_dispatch.c"
tooth reply-stage-never-set T1 "the destination is validated but never staged for the deliver" "$W/t91"

# t92: the deliver's consume deleted — the stage survives the apply, so
# the NEXT reply payload aims at a destination already used (and a
# refusal path leaves it armed for a request that never made it).
seed "$W/t92"
sed -i '/pd->reply_stage_valid = 0;/d' "$W/t92/kernel/chan.c"
tooth reply-stage-never-consumed T2 "the deliver reads the stage but never clears it" "$W/t92"

# t93: the slot-reuse clear deleted — "dead code" is how a reused task
# slot inherits its predecessor's destination, and the next reply
# payload writes into the successor's address space.
seed "$W/t93"
sed -i '/pd->reply_stage_valid = 0;/d' "$W/t93/kernel/process.c"
tooth reply-stage-slotreuse-clear-gone T2 "a reused slot keeps the dead task's staged destination" "$W/t93"

# t94: the staged clamp dropped from the shape gate — a payload larger
# than the first trap allowed would pass shape validation and be copied.
seed "$W/t94"
sed -i 's/!staged || n > slen/!staged/' "$W/t94/kernel/chan.c"
tooth deliver-clamp-gone T3 "the deliver accepts a payload beyond the staged clamp" "$W/t94"

# t95: the writable check deleted — "readable is enough" is how the
# sidecar starts laundering writes through pages the child could not
# write itself (the IN direction's whole new security property).
seed "$W/t95"
sed -i '/if (require_rw && !(e & LAB_PTE_RW)) return -LINUX_EFAULT;/d' "$W/t95/kernel/linux_abi.c"
tooth copyin-writable-check-gone T4 "the copy-IN writes wherever the chain is present+user, read-only or not" "$W/t95"

# t96: the sidecar's bound inverted — ReplyBytes would send whatever it
# likes, and only the kernel's staged clamp would still be holding the
# line (one end load-bearing, which is exactly what T5 forbids).
seed "$W/t96"
sed -i 's/if args\[2\] == 0 || len != want || len > bytes.len() {/if false {/' "$W/t96/user/sidecar/src/linux.rs"
tooth reply-bytes-unbounded T5 "the sidecar sends a payload no out-pointer asked for (or the wrong size)" "$W/t96"

# t97: the payload never sent — an arm answers ReplyBytes and service
# drops the bytes into a plain header, so the deliver sees a SUCCESS with
# an owed payload and refuses -EPROTO (the caller's error instead of its
# old disposition).
seed "$W/t97"
sed -i '/reply_bytes(k, ch_w, rr.tag, num, ret, &bytes\[\.\.len\]);/d' "$W/t97/user/sidecar/src/linux.rs"
tooth reply-payload-never-sent T5 "service() accepts the payload answer and replies a bare 72-byte header" "$W/t97"

# t98: the copy-IN's host-test section deleted — the two-phase order and
# the writable boundary would then be argued from these greps alone,
# every branch reachable only at boot (t83/t89's rule, third seam along).
seed "$W/t98"
sed -i '/linux_fd_reply_plan(/d; /linux_user_copy_in(/d; /linux_reply_shape_ok(/d' "$W/t98/tests/linux_personality_host_test.c"
tooth copyin-unhosttested T6 "the host test no longer drives the reply plan, the copy or the shape predicate" "$W/t98"

# t99: the retired out-pointer ENOSYS re-added to the sig plan — R1's
# negation half: the refusal increment 12 removed must STAY removed, or
# the reply payload is double-trapped and never staged.
seed "$W/t99"
sed -i '/p.refuse = -LINUX_EFAULT;/a\    p.refuse = -LINUX_ENOSYS;' "$W/t99/kernel/linux_abi.c"
tooth retired-oldact-enosys-back R1 "the out-pointer ENOSYS the copy-IN retired reappears in the sig plan" "$W/t99"

# t100: the six-register restore deleted — syscall_return_path hands
# ring-3 kernel scratch in rdi/rsi/rdx/r10/r8/r9 on every syscall: the
# 0x8054bdf wall (musl's `act` in r8 read at 0x88(r8)) back by name.
seed "$W/t100"
sed -i -E '/mov +(rdi|rsi|rdx|r10|r8|r9), +\[gs:0x(50|58|60|68|70|78)\]/d' "$W/t100/arch/x86/syscall.asm"
tooth arg-regs-never-restored U "the return path gives back kernel scratch instead of the caller's registers" "$W/t100"

# t101: the capture half deleted — park_ctx keeps the pushed set but
# not the argument registers, so every resume restores words some later
# trap on that core wrote.
seed "$W/t101"
sed -i '/movq %%gs:0x50, %%rax/d' "$W/t101/kernel/process.c"
tooth park-loses-arg-regs U2 "proc_capture_entry_regs no longer snapshots the argument registers" "$W/t101"

# t102: the resume re-stamp's load deleted — the resume never runs the
# entry stub, so [gs:0x50..] still holds the LAST entry's values and
# the sysret returns them (the store alone ships whatever rax held).
seed "$W/t102"
sed -i '/movq 72(%1), %%rax/d' "$W/t102/kernel/process.c"
tooth resume-restamps-nothing U3 "a resume restores from a block nobody re-stamped" "$W/t102"

# t103: the struct field dropped — the capture has nowhere to put the
# registers and the resume's byte reads walk past the struct.
seed "$W/t103"
sed -i '/uint64_t linux_args\[6\];/d' "$W/t103/kernel/process.h"
tooth parkctx-argfield-gone U4 "CapParkCtx no longer carries the argument registers" "$W/t103"

# t104: the host-test pins deleted — the 72(%1)/112(%1) offsets the
# resume asm indexes by would then be argued from greps alone.
seed "$W/t104"
sed -i '/offsetof(struct CapParkCtx, linux_args) == 72/d' "$W/t104/tests/cap_create_sidecar_host_test.c"
sed -i '/sizeof(struct CapParkCtx) == 120/d' "$W/t104/tests/cap_create_sidecar_host_test.c"
tooth parkctx-layout-unpinned U5 "nothing pins where the resume asm thinks the registers are" "$W/t104"

# t105: the restore written through RAX — the one register the round
# trip must not put back: ring-3 would read a capture word as the
# syscall's result (and the word, not the errno, decides the caller's
# branch).
seed "$W/t105"
sed -i '/mov  rsi, \[gs:0x58\]/a\    mov  rax, [gs:0x58]' "$W/t105/arch/x86/syscall.asm"
tooth restore-clobbers-rax U1 "the return path overwrites the syscall result on its way back" "$W/t105"

# t106: the signal numbers pulled off both attribution lines — tkill's
# pending ask and the killing delivery go back to printing a constant,
# so WHICH signal is again reconstructed from which arms ran instead of
# read off the line that proves it (increment 14's whole addition).
seed "$W/t106"
sed -i 's/fmt_sig_line(&mut ex, b" ok sig=", args\[1\]/fmt_sig_line(&mut ex, b" ok sig=", 0/' "$W/t106/user/sidecar/src/linux.rs"
sed -i 's/fmt_sig_line(&mut ex, b" sig=", kill_sig/fmt_sig_line(&mut ex, b" sig=", 0/' "$W/t106/user/sidecar/src/linux.rs"
tooth signal-number-not-logged R7 "neither line carries the signal number anymore" "$W/t106"

echo
echo "linux_personality_check_smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

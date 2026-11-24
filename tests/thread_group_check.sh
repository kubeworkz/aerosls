#!/usr/bin/env bash
# tests/thread_group_check.sh — E7 TG-1 I5: the source contract of the thread
# group, its clone/futex/openat slice, and the per-thread forward lease.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# I1–I4 are host-tested and live-probed, but each of those checks drives one
# entry point. The contract that makes a cloned sibling work is a set of
# AGREEMENTS ACROSS FILES that no single host test sees:
#
#   - the group, not the descriptor, owns cr3, the cap table/cursors and the
#     one sidecar channel pair;
#   - clone accepts Go's exact six shared flags (plus optional SETTLS) and
#     builds the child's ring-3 context;
#   - the table rows exist with the classes the dispatcher arms;
#   - the forward lease serializes one in-flight request per group, parks a
#     busy sibling WITHOUT posting, restarts it at its own `syscall`, and
#     releases on reply, refusal and exit;
#   - process_find_current names the EXECUTING thread (not merely the first
#     RUNNING one), and channel resolution goes through the group's cap
#     owner;
#   - exactly one place tears down the shared page table and cap table.
#
# A future edit can keep every host test green while breaking one of these
# seams — for example by deleting the lease release on the no-channel path,
# or by letting cap_table_index stop resolving a group member. This guard is
# the cheap, source-only half that names the seam. The live half is the three
# native probes plus the E7 gate, and the mutation teeth for this file are in
# tests/thread_group_check_smoke.sh.
#
# Each clause names the consequence, not only the pattern, because a failure
# here has to say what a boot would lose. The file declarations at the top of
# the clause body (`NAME=path`) are also the seed contract for the smoke: it
# checks that its copies carry every file the guard reads.
#
# Source-only: no build, no boot, no toolchain, so it carries no GUARD-KIND.
#
# Run:  bash tests/thread_group_check.sh
#       THREAD_GROUP_CHECK_ROOT=<dir> to check a seeded tree (smoke).
# Exit: 0 pass, 1 violated.
set -uo pipefail

ROOT="${THREAD_GROUP_CHECK_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 2

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails+1)); }

# need FILE PATTERN CLAUSE DESCRIPTION — one fixed-string read.
need() {
    local f="$1" pat="$2" name="$3" desc="$4"
    if [ ! -f "$f" ]; then
        bad "$name. $desc ($f missing)"
    elif grep -qF -- "$pat" "$f"; then
        ok "$name. $desc"
    else
        bad "$name. $desc (missing '$pat' in $f)"
    fi
}

# needre FILE REGEX CLAUSE DESCRIPTION — one extended-regex read.
needre() {
    local f="$1" re="$2" name="$3" desc="$4"
    if [ ! -f "$f" ]; then
        bad "$name. $desc ($f missing)"
    elif grep -Eq -- "$re" "$f"; then
        ok "$name. $desc"
    else
        bad "$name. $desc (no /$re/ in $f)"
    fi
}

# all_fixed FILE CLAUSE DESCRIPTION PATTERN... — one clause over many reads,
# listing every pattern the file no longer carries.
all_fixed() {
    local f="$1" name="$2" desc="$3" missing=""
    shift 3
    for pat in "$@"; do
        [ -f "$f" ] && grep -qF -- "$pat" "$f" || missing="$missing [$pat]"
    done
    if [ -z "$missing" ]; then
        ok "$name. $desc"
    else
        bad "$name. $desc (missing:$missing)"
    fi
}

# all_regex FILE CLAUSE DESCRIPTION REGEX...
all_regex() {
    local f="$1" name="$2" desc="$3" missing=""
    shift 3
    for re in "$@"; do
        [ -f "$f" ] && grep -Eq -- "$re" "$f" || missing="$missing [/$re/]"
    done
    if [ -z "$missing" ]; then
        ok "$name. $desc"
    else
        bad "$name. $desc (missing:$missing)"
    fi
}

LTH=kernel/linux_thread.h
LTC=kernel/linux_thread.c
LAH=kernel/linux_abi.h
LAC=kernel/linux_abi.c
PDH=kernel/process.h
PDC=kernel/process.c
CAPC=kernel/cap.c
DISP=kernel/syscall_dispatch.c
CAN=kernel/chan.c
LRS=user/sidecar/src/linux.rs
GHT=tests/linux_thread_group_host_test.c
CHT=tests/linux_clone_host_test.c
FHT=tests/linux_futex_host_test.c
SHT=tests/linux_syscall_table_host_test.c
PHT=tests/linux_personality_host_test.c
TCP=tests/linux_thread_probe_check.sh
FCP=tests/linux_futex_probe_check.sh
OCP=tests/linux_openat_probe_check.sh
TFIX=user/sidecar/fixtures/e7_thread_probe.S
FFIX=user/sidecar/fixtures/e7_futex_probe.S
OFIX=user/sidecar/fixtures/e7_openat_probe.S
LRE=user/sidecar/src/entry.rs
LRL=user/sidecar/src/lib.rs

# ─── A. Inputs ─────────────────────────────────────────────────────────────
missing=""
for f in "$LTH" "$LTC" "$LAH" "$LAC" "$PDH" "$PDC" "$CAPC" "$DISP" "$CAN" \
         "$LRS" "$GHT" "$CHT" "$FHT" "$SHT" "$PHT" \
         "$TCP" "$FCP" "$OCP" "$TFIX" "$FFIX" "$OFIX" "$LRE" "$LRL"; do
    [ -f "$f" ] || missing="$missing $f"
done
if [ -z "$missing" ]; then
    ok "A1. every file this guard reads exists"
else
    bad "A1. guard input missing:$missing"
fi

# ─── B. The group owns the shared state, and one site destroys it ─────────
# A field that drifts back onto the descriptor is a per-thread copy of shared
# state: two heaps, two cursor pairs, two cr3s, or a lease state per thread.
all_regex "$LTH" "B1" "the group struct owns cr3, the shared cursors and the lease state" \
    'uint64_t[[:space:]]+cr3;' \
    'uint64_t[[:space:]]+heap_base;' \
    'uint64_t[[:space:]]+heap_limit;' \
    'uint64_t[[:space:]]+brk_cur;' \
    'uint64_t[[:space:]]+mmap_top;' \
    'uint64_t[[:space:]]+image_lo;' \
    'uint64_t[[:space:]]+image_hi;' \
    'uint64_t[[:space:]]+dir_pos;' \
    'uint8_t[[:space:]]+live_threads;' \
    'uint32_t[[:space:]]+leader_pid;' \
    'uint32_t[[:space:]]+cap_owner_pid;' \
    'uint8_t[[:space:]]+fwd_busy;' \
    'uint32_t[[:space:]]+fwd_owner_tid;' \
    'uint32_t[[:space:]]+fwd_wait_mask;'

all_fixed "$LTC" "B2" "create stamps the group id, leader, cap owner, root and identity" \
    'g->group_id = pd->pid;' \
    'g->leader_pid = pd->pid;' \
    'g->cap_owner_pid = pd->pid;' \
    'g->cr3 = pd->cr3;' \
    'g->live_threads = 1;' \
    'pd->tid = pd->pid;' \
    'pd->tgid = pd->pid;'

all_fixed "$LTC" "B3" "join gives the child the group's root, identity and a clean resume slot" \
    'child->cr3 = g->cr3;' \
    'child->tid = child->pid;' \
    'child->tgid = g->group_id;' \
    'child->sysret_result = 0;'

all_fixed "$LTC" "B4" "the group's ONE destroy tears down the cap table and page table" \
    'cap_table_teardown(g->cap_owner_pid);' \
    'user_destroy_page_table(g->cr3);'
need "$PDC" "linux_group_destroy(g);" "B4b" \
    "every group teardown routes through linux_group_destroy"

# ─── C. clone's accepted shape and the child context ──────────────────────
all_regex "$LTH" "C1" "the six Go shared flags are defined" \
    '#define[[:space:]]+LINUX_CLONE_VM[[:space:]]+0x00000100ull' \
    '#define[[:space:]]+LINUX_CLONE_FS[[:space:]]+0x00000200ull' \
    '#define[[:space:]]+LINUX_CLONE_FILES[[:space:]]+0x00000400ull' \
    '#define[[:space:]]+LINUX_CLONE_SIGHAND[[:space:]]+0x00000800ull' \
    '#define[[:space:]]+LINUX_CLONE_THREAD[[:space:]]+0x00010000ull' \
    '#define[[:space:]]+LINUX_CLONE_SYSVSEM[[:space:]]+0x00040000ull'

go_block="$(sed -n '/#define LINUX_CLONE_GO_FLAGS/,/LINUX_CLONE_THREAD)/p' "$LTH" 2>/dev/null)"
if printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_VM' &&
   printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_FS' &&
   printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_FILES' &&
   printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_SIGHAND' &&
   printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_SYSVSEM' &&
   printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_THREAD' &&
   ! printf '%s\n' "$go_block" | grep -qF 'LINUX_CLONE_SETTLS'; then
    ok "C1b. the mandatory mask is exactly the six shared flags (SETTLS stays optional)"
else
    bad "C1b. LINUX_CLONE_GO_FLAGS no longer holds exactly VM|FS|FILES|SIGHAND|SYSVSEM|THREAD without SETTLS"
fi

need "$LTH" 'LINUX_CLONE_ALLOWED  (LINUX_CLONE_GO_FLAGS | LINUX_CLONE_SETTLS)' "C2" \
    "the accepted set is the mandatory six plus optional SETTLS"
need "$LTC" 'if (flags & ~LINUX_CLONE_ALLOWED) return -LINUX_EINVAL;' "C2b" \
    "clone refuses an unaccepted flag before touching any table"
need "$LTC" 'if ((flags & LINUX_CLONE_GO_FLAGS) != LINUX_CLONE_GO_FLAGS)' "C2c" \
    "clone requires every shared flag, not a subset"
all_fixed "$LTC" "C3" "the child's first ring-3 context is rax=0 at the parent RIP on its own stack" \
    'ctx->rax   = 0;' \
    'ctx->rip   = parent_rip;' \
    'ctx->rsp   = child_stack;'

# ─── D. The measured rows and the openat path plan ────────────────────────
all_regex "$LAC" "D1" "the six TG-1 table rows carry their classes" \
    '\{[[:space:]]*39,[[:space:]]*LSC_TASK,' \
    '\{[[:space:]]*56,[[:space:]]*LSC_THREAD,' \
    '\{[[:space:]]*60,[[:space:]]*LSC_TASK,' \
    '\{[[:space:]]*186,[[:space:]]*LSC_TASK,' \
    '\{[[:space:]]*202,[[:space:]]*LSC_FUTEX,' \
    '\{[[:space:]]*257,[[:space:]]*LSC_FD,'

need "$LAH" '#define LINUX_OPENAT      257u' "D2" \
    "openat's number is owned in the ABI header"
need "$LAC" 'p.src  = (num == LINUX_OPENAT) ? args[1] : args[0];' "D2b" \
    "openat's pathname is args[1], not the dirfd"
need "$LRS" 'const LINUX_AT_FDCWD: i64 = -100;' "D3" \
    "the sidecar names Linux's AT_FDCWD value"
need "$LRS" 'LINUX_OPEN | LINUX_OPENAT => {' "D3b" \
    "openat shares open's VFS arm"
need "$LRS" 'if at && args[0] as i64 != LINUX_AT_FDCWD {' "D3c" \
    "a non-AT_FDCWD openat is refused by name, never resolved against a guess"
need "$LRS" 'let base = if at { 1 } else { 0 };' "D3d" \
    "openat reads flags/mode one argument above open's"
need "$LRS" 'LINUX_OPEN | LINUX_OPENAT | LINUX_STAT | LINUX_LSTAT | LINUX_UNLINK |' "D4" \
    "the request-range bound accepts openat's path payload"
need "$LAH" '#define SYS_LINUX_LEASE_RESUME 0x4C4C5345u' "D5" \
    "the lease park sentinel is the named LLSE value"

# ─── E. Per-thread identity and group-aware channel resolution ────────────
need "$PDC" 'proc_table[i].syscall_stack_top == per_cpu_data[0].kernel_rsp' "E1" \
    "process_find_current matches the executing thread's syscall-stack top"
need "$CAN" 'pid = proc_group_cap_owner(pid);' "E2" \
    "chan_table_for_pid resolves a cloned sibling to the group's cap owner"
need "$CAPC" 'pid = proc_group_cap_owner(pid);' "E3" \
    "cap_table_index resolves a group member to the leader's table"

# ─── F. The per-thread forward lease ──────────────────────────────────────
all_fixed "$LTH" "F1" "the lease's six transitions are declared in the group header" \
    'int linux_group_fwd_acquire(struct LinuxThreadGroup* g, uint32_t tid);' \
    'int linux_group_fwd_wait(struct LinuxThreadGroup* g, uint32_t slot);' \
    'int linux_group_fwd_cancel_wait(struct LinuxThreadGroup* g, uint32_t slot);' \
    'int linux_group_fwd_release(struct LinuxThreadGroup* g, uint32_t tid);' \
    'int linux_group_fwd_grant(struct LinuxThreadGroup* g, uint32_t tid);' \
    'uint32_t linux_group_fwd_next_waiter(struct LinuxThreadGroup* g);'

need "$LTC" 'return (g->fwd_owner_tid == tid) ? 0 : 1;' "F2" \
    "a busy acquire reports the conflict instead of granting a second owner"
need "$LTC" 'g->fwd_wait_mask |= (1u << slot);' "F3" \
    "a busy sibling records its wait bit"
need "$PDC" 'cur->park_syscall = SYS_LINUX_LEASE_RESUME;' "F3b" \
    "the waiting sibling arms the lease-restart sentinel"
need "$PDC" 'cur->park_req     = num;' "F3c" \
    "the waiting sibling remembers the syscall to restart"
need "$LTC" 'for (uint32_t i = 0; i < PROC_MAX; i++) {' "F4" \
    "next_waiter scans from the lowest slot"
need "$LTC" 'g->fwd_wait_mask &= ~(1u << i);' "F4b" \
    "next_waiter removes the slot it returns"
need "$LTC" 'if (!g->fwd_busy || g->fwd_owner_tid != tid) return -1;' "F5" \
    "only the owner may release the lease"
all_fixed "$PDC" "F5b" "release hands the lease to a live waiter and restarts it" \
    'linux_group_fwd_next_waiter(g)' \
    'pd->park_syscall != SYS_LINUX_LEASE_RESUME' \
    'linux_group_fwd_grant(g, pd->tid) != 0' \
    'linux_forward_lease_restart(pd);'
all_fixed "$PDC" "F6" "a granted waiter restarts at the syscall instruction, not after it" \
    'linux_user_copy_out(op, pd->cr3, rip - 2, 2) == 0' \
    'op[0] == 0x0F && op[1] == 0x05' \
    'linux_park_return_frame(pd, num, rip - 2);'

acq_line="$(grep -n 'linux_group_fwd_acquire(lg, cur->tid)' "$DISP" 2>/dev/null | head -n 1 | cut -d: -f1)"
stage_line="$(grep -n 'cur->reply_stage_num   = num;' "$DISP" 2>/dev/null | head -n 1 | cut -d: -f1)"
post_line="$(grep -n 'k_linux_forward(cur->pid' "$DISP" 2>/dev/null | head -n 1 | cut -d: -f1)"
if [ -n "$acq_line" ] && [ -n "$stage_line" ] && [ -n "$post_line" ] &&
   [ "$acq_line" -lt "$stage_line" ] && [ "$stage_line" -lt "$post_line" ]; then
    ok "F7. the dispatcher acquires the lease after every refusal, before staging and posting"
else
    bad "F7. lease acquire is not between the first-trap refusals and the post/arm (acquire=${acq_line:-none} stage=${stage_line:-none} post=${post_line:-none})"
fi

release_sites="$(grep -c 'linux_forward_lease_release(cur);' "$DISP" 2>/dev/null || true)"
if [ "${release_sites:-0}" -ge 3 ]; then
    ok "F8. the reply, the missing-channel reply and a refused post each release the lease"
else
    bad "F8. only ${release_sites:-0} dispatcher release site(s), expected the reply, the no-channel path and the refused post"
fi

all_fixed "$PDC" "F9" "descriptor teardown cancels a waiter and releases an owner's lease" \
    'linux_group_fwd_cancel_wait(g, (uint32_t)(pd - proc_table));' \
    'if (g->fwd_busy && g->fwd_owner_tid == pd->tid)'

# ─── G. The live probes and the host tests are still wired ────────────────
all_fixed "$TFIX" "G1" "the two-thread probe still carries its live markers" \
    'E7TG-START' 'E7TG-CHILD-OK' 'E7TG-SHARED-OK' 'E7TG-FAIL'
all_fixed "$FFIX" "G1b" "the futex probe still carries its live markers" \
    'E7FX-START' 'E7FX-WAIT-OK' 'E7FX-TIMEOUT-OK' 'E7FX-OK'
all_fixed "$OFIX" "G1c" "the openat probe still carries its live markers" \
    'E7OA-START' 'E7OA-OPEN-OK' 'E7OA-CLOSE-OK' 'E7OA-OK'
all_fixed "$LRE" "G2" "the sidecar boot spawns all three native probes" \
    'crate::e7_thread_probe::spawn' \
    'crate::e7_futex_probe::spawn' \
    'crate::e7_openat_probe::spawn'
all_fixed "$LRL" "G2b" "the sidecar declares all three probe modules" \
    'pub mod e7_thread_probe;' \
    'pub mod e7_futex_probe;' \
    'pub mod e7_openat_probe;'
all_fixed "$GHT" "G3" "the group host test still drives the lease state machine and coordinated restore release" \
    'linux_group_fwd_acquire' 'linux_group_fwd_next_waiter' 'linux_group_fwd_release' \
    'linux_group_release_restored' 'a futex waiter stays BLOCKED for its wake'
[ -s "$CHT" ] && ok "G3b. the clone host test exists" || bad "G3b. tests/linux_clone_host_test.c is missing"
[ -s "$FHT" ] && ok "G3c. the futex host test exists" || bad "G3c. tests/linux_futex_host_test.c is missing"
[ -s "$SHT" ] && ok "G3d. the ABI table host test exists" || bad "G3d. tests/linux_syscall_table_host_test.c is missing"
[ -s "$PHT" ] && ok "G3e. the personality host test exists" || bad "G3e. tests/linux_personality_host_test.c is missing"

# ─── Verdict ──────────────────────────────────────────────────────────────
echo "thread_group_check: $fails failure(s)"
[ "$fails" -eq 0 ]

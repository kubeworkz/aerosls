#!/usr/bin/env bash
# tests/thread_group_check_smoke.sh — mutation teeth for
# tests/thread_group_check.sh.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# A source guard that has only ever seen a correct tree is a guard nobody has
# watched fail. Each tooth here deletes or distorts exactly one of the
# agreements the guard names — a field drifting back onto the descriptor, a
# clone flag dropped from the mask, the lease restart moved after `syscall`,
# the group destroy losing its page-table half — and requires the guard to
# exit 1 SAYING WHICH CLAUSE. The mutations are plausible future edits
# ("clean up" an unused field, simplify a mask, remove a redundant release),
# not artificial corruption.
#
# The mutations run on COPIES (seed() below): the real tree is never touched.
# The baseline run on the copy doubles as the seed-fidelity control; the
# seed coverage loop refuses a copy that lacks a file the guard reads, which
# is how the personality smoke caught clauses that failed only on copies.
#
# Run:  bash tests/thread_group_check_smoke.sh
# GUARD-KIND: host (bash only; no build, no QEMU, no toolchain).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

GUARD=tests/thread_group_check.sh
FILES=(
    kernel/linux_thread.h
    kernel/linux_thread.c
    kernel/linux_abi.h
    kernel/linux_abi.c
    kernel/process.h
    kernel/process.c
    kernel/cap.c
    kernel/syscall_dispatch.c
    kernel/chan.c
    user/sidecar/src/linux.rs
    user/sidecar/src/entry.rs
    user/sidecar/src/lib.rs
    tests/linux_thread_group_host_test.c
    tests/linux_clone_host_test.c
    tests/linux_futex_host_test.c
    tests/linux_syscall_table_host_test.c
    tests/linux_personality_host_test.c
    tests/linux_thread_probe_check.sh
    tests/linux_futex_probe_check.sh
    tests/linux_openat_probe_check.sh
    user/sidecar/fixtures/e7_thread_probe.S
    user/sidecar/fixtures/e7_futex_probe.S
    user/sidecar/fixtures/e7_openat_probe.S
)

for f in "${FILES[@]}" "$GUARD"; do
    [ -f "$f" ] || { echo "ABORT: $f missing." >&2; exit 2; }
done

# ─── The seed covers every file the GUARD reads ────────────────────────────
# The guard declares its inputs as `NAME=path` lines; this list is maintained
# by hand, so hold it to that declaration. A read added to the guard without
# a line above fails here BY NAME, before any tooth runs.
unseeded=""
seeded=" ${FILES[*]} "
for p in $(grep -E '^[A-Z][A-Z0-9_]*=[a-z]' "$GUARD" | cut -d= -f2); do
    case "$seeded" in
        *" $p "*) ;;
        *) unseeded="$unseeded $p" ;;
    esac
done
if [ -n "$unseeded" ]; then
    echo "ABORT: the seed does not carry every file the guard reads:$unseeded" >&2
    echo "       Add them to FILES above: a clause whose input the copy lacks" >&2
    echo "       fails on the copy and passes on the real tree." >&2
    exit 2
fi

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

run_guard() { THREAD_GROUP_CHECK_ROOT="$1" bash "$GUARD" 2>&1; }

pass=0
fail=0

# tooth NAME CLAUSE DESCRIPTION DIR — the guard must exit 1 and name CLAUSE.
tooth() {
    local name="$1" clause="$2" desc="$3" d="$4" out rc named=0
    # VACUITY GUARD: a sed whose pattern stopped matching leaves the copy
    # pristine; the guard then passes and the tooth would blame the guard.
    if diff -r "$W/base" "$d" >/dev/null 2>&1; then
        echo "TOOTH FAIL $name — mutation did not apply: the seeded copy is byte-identical to the baseline"
        fail=$((fail+1))
        return
    fi
    out="$(run_guard "$d")"; rc=$?
    # A case glob, not `printf | grep -q`: under pipefail grep -q can take
    # SIGPIPE and report 141, scoring a named failure as un-bitten.
    case "$out" in
        *"FAIL: $clause."*) named=1 ;;
    esac
    if [ "$rc" -eq 1 ] && [ "$named" -eq 1 ]; then
        echo "TOOTH OK   $name — guard exited 1 naming $clause ($desc)"
        pass=$((pass+1))
    else
        echo "TOOTH FAIL $name — guard rc=$rc (want 1), clause '$clause' not named"
        printf '%s\n' "$out" | grep "FAIL:" | head -4 | sed 's/^/           /'
        fail=$((fail+1))
    fi
}

echo "thread_group_check_smoke — teeth for the TG-1 thread-group source guard"
echo "========================================================================"

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

# ─── B: a shared field drifts back onto the descriptor ────────────────────
cp -a "$W/base" "$W/field"
sed -i '/uint64_t cr3;/d' "$W/field/kernel/linux_thread.h"
tooth field-removed B1 "the group's page-table root removed from the group struct" "$W/field"

# ─── B: the group destroy loses its page-table half ──────────────────────
cp -a "$W/base" "$W/destroy"
sed -i '/user_destroy_page_table(g->cr3);/d' "$W/destroy/kernel/linux_thread.c"
tooth destroy-half B4 "the one group destroy no longer frees the shared cr3" "$W/destroy"

# ─── C: a required clone flag is dropped from the mask ───────────────────
cp -a "$W/base" "$W/flag"
sed -i '/#define LINUX_CLONE_SIGHAND/d' "$W/flag/kernel/linux_thread.h"
tooth clone-flag C1 "CLONE_SIGHAND removed from the accepted thread shape" "$W/flag"

# ─── C: clone accepts an arbitrary flag word again ────────────────────────
cp -a "$W/base" "$W/flags"
sed -i 's/if (flags \& ~LINUX_CLONE_ALLOWED) return -LINUX_EINVAL;/if (0) return -LINUX_EINVAL;/' \
    "$W/flags/kernel/linux_thread.c"
tooth clone-mask C2b "the unknown-flag refusal removed before the table mutation" "$W/flags"

# ─── D: the futex row is dropped from the ABI table ─────────────────────
cp -a "$W/base" "$W/row"
sed -i '/{ 202, LSC_FUTEX,/d' "$W/row/kernel/linux_abi.c"
tooth table-row D1 "the 202 futex row deleted from linux_syscall_table" "$W/row"

# ─── D: openat reads the dirfd as its pathname ───────────────────────────
cp -a "$W/base" "$W/path"
sed -i 's/? args\[1\] : args\[0\]/? args[2] : args[0]/' "$W/path/kernel/linux_abi.c"
tooth openat-path D2b "openat's path plan reads args[2] instead of args[1]" "$W/path"

# ─── D: a non-AT_FDCWD openat is no longer refused by name ───────────────
cp -a "$W/base" "$W/dirfd"
sed -i 's/if at \&\& args\[0\] as i64 != LINUX_AT_FDCWD {/if false \&\& args[0] as i64 != LINUX_AT_FDCWD {/' \
    "$W/dirfd/user/sidecar/src/linux.rs"
tooth dirfd-refusal D3c "the sidecar's AT_FDCWD-only check disabled" "$W/dirfd"

# ─── E: the channel lookup stops resolving the group's cap owner ─────────
cp -a "$W/base" "$W/chan"
sed -i '/pid = proc_group_cap_owner(pid);/d' "$W/chan/kernel/chan.c"
tooth chan-owner E2 "chan_table_for_pid searches the sibling's own pid again" "$W/chan"

# ─── E: current-thread lookup falls back to the first-RUNNING scan ───────
cp -a "$W/base" "$W/current"
sed -i 's/per_cpu_data\[0\]\.kernel_rsp/per_cpu_data[1].kernel_rsp/' "$W/current/kernel/process.c"
tooth current-thread E1 "process_find_current no longer matches the executing stack top" "$W/current"

# ─── F: the waiter's mask bit is never set ───────────────────────────────
cp -a "$W/base" "$W/mask"
sed -i '/g->fwd_wait_mask |= (uint8_t)(1u << slot);/d' "$W/mask/kernel/linux_thread.c"
tooth lease-mask F3 "a busy sibling parks without recording its wait bit" "$W/mask"

# ─── F: the restart lands after the syscall instead of on it ─────────────
cp -a "$W/base" "$W/restart"
sed -i 's/linux_park_return_frame(pd, num, rip - 2);/linux_park_return_frame(pd, num, rip);/' \
    "$W/restart/kernel/process.c"
tooth lease-restart F6 "a granted waiter resumes after the syscall instruction instead of restarting it" "$W/restart"

# ─── F: the dispatcher stops taking the lease before the post ────────────
cp -a "$W/base" "$W/acquire"
sed -i '/lrc = linux_group_fwd_acquire(lg, cur->tid);/d' "$W/acquire/kernel/syscall_dispatch.c"
tooth dispatch-acquire F7 "the dispatcher posts without acquiring the group's one forward slot" "$W/acquire"

# ─── F: a departing waiter is not cancelled ──────────────────────────────
cp -a "$W/base" "$W/cancel"
sed -i '/linux_group_fwd_cancel_wait(g, (uint32_t)(pd - proc_table));/d' "$W/cancel/kernel/process.c"
tooth teardown-cancel F9 "a departing thread leaves its wait bit for the next grant" "$W/cancel"

# ─── Verdict ──────────────────────────────────────────────────────────────
echo
echo "thread_group_check_smoke: $pass tooth(s) passed, $fail failed"
[ "$fail" -eq 0 ]

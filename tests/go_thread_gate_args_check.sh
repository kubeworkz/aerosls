#!/usr/bin/env bash
# tests/go_thread_gate_args_check.sh — TG-0b: the argument-level census is
# load-bearing, and the current tree still matches the gate document.
#
# This guard is source-only. It reads the committed argument artifact written
# by tools/go_thread_gate_census.sh (no Go, no strace, no boot) and asserts the
# measured shapes the TG-1 design depends on:
#
#   - clone: Go's exact seven-flag thread shape, no CLONE_CHILD_CLEARTID;
#   - futex: private WAIT/WAKE only, with the measured timeout forms;
#   - epoll_pwait: zero-timeout probes, no sigmask;
#   - openat: AT_FDCWD for the fixture's input and output;
#   - rt_sigaction: handler installs are record-only and carry the measured
#     SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO flags; queries return the
#     old action.
#
# It also pins the current tree's reconciliation: the four netpoll rows exist,
# the sidecar's record-only arm and netpoll unit test are present, and each of
# the six TG-1 rows (clone/futex/gettid/getpid/exit/openat) is either still an
# open gap or has landed with the class §5.1 assigns it. The six close in the
# plan's increments (I2: clone, futex; I4: gettid, getpid, exit, openat), so a
# landed row is a passed phase, not a stale gap; a landed row with the wrong
# class is the failure.
#
# Run:  bash tests/go_thread_gate_args_check.sh
#       GO_THREAD_GATE_ARGS_ARTIFACT=<path> to test a mutated artifact.
# Exit: 0 pass, 1 violated, 2 the artifact is missing.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

ART="${GO_THREAD_GATE_ARGS_ARTIFACT:-tools/go_thread_gate_args_census.txt}"
TABLE=kernel/linux_abi.c
LRS=user/sidecar/src/linux.rs
DOC=docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md

pass=0
fail=0
ok()  { echo "ok:   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }

if [ ! -f "$ART" ]; then
    echo "ABORT: $ART missing — TG-0b argument census has not been generated." >&2
    exit 2
fi

# ─── Provenance and the one-run contract ──────────────────────────────────
grep -q '^# candidate: go_thread_gate' "$ART" \
    && ok "artifact names the pinned fixture" \
    || bad "artifact does not name the pinned fixture"
grep -q '^# generated-by: tools/go_thread_gate_census.sh' "$ART" \
    && ok "artifact names its generator" \
    || bad "artifact does not name tools/go_thread_gate_census.sh"
grep -q '^# source-sha256: main.go=d25479aefc54378ae09a6da62e69eb77329f4acd51404d12dc5d68008ce6c6e9 go.mod=47b4f13c9c258c87c8e51e97c78239391dfb248fb1f16c92e84658fdd24f7300' "$ART" \
    && ok "artifact pins the fixture source hashes" \
    || bad "artifact is missing the measured fixture source hashes"
grep -q '^# toolchain: go version go1.25.1 ' "$ART" \
    && ok "artifact is pinned to Go 1.25.1" \
    || bad "artifact is not pinned to Go 1.25.1"
grep -q '^# toolchain-archive-sha256: 7716a0d940a0f6ae8e1f3b3f4f36299dc53e31b16840dbd171254312c41ca12e (verified from ' "$ART" \
    && ok "artifact verifies the pinned Go 1.25.1 tarball sha256" \
    || bad "artifact does not verify the pinned toolchain archive sha256"
grep -q '^# binary-sha256: a1348ca7daf693827a4a35bf437b9f1fb82dfd5b0d9cbf0dba7153388cdc8f28' "$ART" \
    && ok "artifact pins the measured fixture binary sha256" \
    || bad "artifact is missing the measured fixture binary sha256"
grep -q '^# note: PIDs are stripped; addresses and handler words are one run, not the contract\.' "$ART" \
    && ok "artifact states the one-run address caveat" \
    || bad "artifact does not state the one-run address caveat"

# ─── Clone: the exact Go thread shape ─────────────────────────────────────
clone_lines="$(grep '^clone(' "$ART" || true)"
clone_count="$(printf '%s\n' "$clone_lines" | grep -c '^clone(' || true)"
[ "${clone_count:-0}" -ge 2 ] \
    && ok "at least two clone rows are recorded ($clone_count)" \
    || bad "fewer than two clone rows are recorded"

clone_expected='CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS'
clone_header="$(sed -n 's/^# clone: [0-9][0-9]* rows; flags: //p' "$ART")"
[ "$clone_header" = "$clone_expected" ] \
    && ok "artifact summary records the exact seven-flag clone shape" \
    || bad "artifact clone summary is '$clone_header', expected '$clone_expected'"
clone_shape_bad=0
while IFS= read -r line; do
    flags="${line#*flags=}"
    flags="${flags%%,*}"
    flags="${flags%% *}"
    [ "$flags" = "$clone_expected" ] || clone_shape_bad=1
done <<< "$clone_lines"
[ "$clone_shape_bad" -eq 0 ] \
    && ok "every clone row has exactly the measured seven-flag thread shape" \
    || bad "a clone row does not have exactly the measured seven-flag thread shape"

# ─── Futex: private WAIT/WAKE only, relative timeout measured ─────────────
futex_lines="$(grep '^futex(' "$ART" || true)"
futex_count="$(printf '%s\n' "$futex_lines" | grep -c '^futex(' || true)"
[ "${futex_count:-0}" -ge 2 ] \
    && ok "at least two futex rows are recorded ($futex_count)" \
    || bad "fewer than two futex rows are recorded"
futex_expected='FUTEX_WAIT_PRIVATE;FUTEX_WAKE_PRIVATE'
futex_ops="$(printf '%s\n' "$futex_lines" | sed -n 's/^futex([^,]*, \([^,]*\),.*/\1/p' | sort -u | paste -sd ';' -)"
[ "$futex_ops" = "$futex_expected" ] \
    && ok "only the measured private futex operations are present" \
    || bad "futex ops are '$futex_ops', expected '$futex_expected'"
if printf '%s\n' "$futex_lines" | grep -q 'tv_nsec='; then
    rel_bad="$(printf '%s\n' "$futex_lines" | grep 'tv_nsec=' | grep -Evc 'tv_sec=[0-9]+, tv_nsec=[0-9]+' || true)"
    [ "${rel_bad:-1}" = 0 ] \
        && ok "observed futex timeouts are relative timespecs" \
        || bad "a futex timeout is not a relative timespec"
else
    ok "no relative futex timeout was exercised in this run (one-run shape)"
fi
printf '%s\n' "$futex_lines" | grep -q 'NULL' \
    && ok "a NULL (forever) futex timeout is recorded" \
    || bad "no NULL futex timeout is recorded"

# ─── epoll_pwait: the measured startup probe is zero-timeout ──────────────
epoll_lines="$(grep '^epoll_pwait(' "$ART" || true)"
epoll_count="$(printf '%s\n' "$epoll_lines" | grep -c '^epoll_pwait(' || true)"
[ "${epoll_count:-0}" -ge 1 ] \
    && ok "at least one epoll_pwait probe is recorded ($epoll_count)" \
    || bad "no epoll_pwait row is recorded"
if printf '%s\n' "$epoll_lines" | grep -v ', 0, NULL, 0)' >/dev/null; then
    bad "an epoll_pwait row has a non-zero timeout or a sigmask"
else
    ok "every epoll_pwait row is timeout 0 with sigmask NULL"
fi

# ─── openat: AT_FDCWD and the measured copy flags ─────────────────────────
openat_lines="$(grep '^openat(' "$ART" || true)"
openat_count="$(printf '%s\n' "$openat_lines" | grep -c '^openat(' || true)"
[ "${openat_count:-0}" -ge 2 ] \
    && ok "openat rows are recorded ($openat_count)" \
    || bad "fewer than two openat rows are recorded"
if printf '%s\n' "$openat_lines" | grep -v 'AT_FDCWD' >/dev/null; then
    bad "an openat row does not use AT_FDCWD"
else
    ok "every openat row uses AT_FDCWD"
fi
printf '%s\n' "$openat_lines" | grep -q 'openat(AT_FDCWD, "in\.bin", O_RDONLY|O_CLOEXEC' \
    && ok "the fixture opens its input O_RDONLY|O_CLOEXEC" \
    || bad "the fixture's input openat shape is missing"
printf '%s\n' "$openat_lines" | grep -q 'openat(AT_FDCWD, "out\.bin", O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC' \
    && ok "the fixture creates its output O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC" \
    || bad "the fixture's output openat shape is missing"

# ─── rt_sigaction: record-only installs with the measured flags ───────────
sig_lines="$(grep '^rt_sigaction(' "$ART" || true)"
sig_count="$(printf '%s\n' "$sig_lines" | grep -c '^rt_sigaction(' || true)"
[ "${sig_count:-0}" -ge 2 ] \
    && ok "rt_sigaction rows are recorded ($sig_count)" \
    || bad "fewer than two rt_sigaction rows are recorded"
sig_install_lines="$(printf '%s\n' "$sig_lines" | grep '^rt_sigaction([^,]*, {sa_handler=' || true)"
sig_install_count="$(printf '%s\n' "$sig_install_lines" | grep -c '^rt_sigaction(' || true)"
[ "${sig_install_count:-0}" -ge 1 ] \
    && ok "handler installs are recorded ($sig_install_count)" \
    || bad "no handler install row is recorded"
printf '%s\n' "$sig_lines" | grep -q 'NULL, {sa_handler=' \
    && ok "an rt_sigaction query row is recorded" \
    || bad "no rt_sigaction query row is recorded"
if printf '%s\n' "$sig_install_lines" | grep -vF 'sa_flags=SA_RESTORER|SA_ONSTACK|SA_RESTART|SA_SIGINFO, sa_restorer=0x' >/dev/null; then
    bad "an rt_sigaction install lacks the measured SA_* flags or restorer"
else
    ok "every rt_sigaction install carries the measured SA_* flags and restorer"
fi
if printf '%s\n' "$sig_install_lines" | grep -vF ', 8) = 0' >/dev/null; then
    bad "an rt_sigaction install uses a non-8 sigsetsize"
else
    ok "every rt_sigaction install uses sigsetsize 8"
fi

# ─── Current-tree reconciliation (§7's status) ────────────────────────────
grep -q '{ 233, LSC_FD' "$TABLE" \
    && ok "netpoll epoll_ctl (233) is classed in the current table" \
    || bad "netpoll epoll_ctl (233) is missing from the table"
grep -q '{ 281, LSC_FD' "$TABLE" \
    && ok "netpoll epoll_pwait (281) is classed in the current table" \
    || bad "netpoll epoll_pwait (281) is missing from the table"
grep -q '{ 290, LSC_FD' "$TABLE" \
    && ok "netpoll eventfd2 (290) is classed in the current table" \
    || bad "netpoll eventfd2 (290) is missing from the table"
grep -q '{ 291, LSC_FD' "$TABLE" \
    && ok "netpoll epoll_create1 (291) is classed in the current table" \
    || bad "netpoll epoll_create1 (291) is missing from the table"
grep -Eq '^[[:space:]]*\{[[:space:]]*13,[[:space:]]*LSC_SIG' "$TABLE" \
    && ok "record-only rt_sigaction (13) remains classed LSC_SIG" \
    || bad "rt_sigaction (13) is no longer classed LSC_SIG"
grep -Eq '^[[:space:]]*\{[[:space:]]*231,[[:space:]]*LSC_EXIT' "$TABLE" \
    && ok "exit_group (231) remains classed LSC_EXIT" \
    || bad "exit_group (231) is no longer classed LSC_EXIT"
grep -Eq '^[[:space:]]*\{[[:space:]]*218,[[:space:]]*LSC_LOCAL' "$TABLE" \
    && ok "set_tid_address (218) remains single-threaded LSC_LOCAL" \
    || bad "set_tid_address (218) is no longer classed LSC_LOCAL"
grep -q 'sig\.act\[args\[0\] as usize\] = act;' "$LRS" \
    && ok "the sidecar records the rt_sigaction action (record-only arm exists)" \
    || bad "the sidecar's record-only rt_sigaction arm is missing"
grep -q 'fn eventfd_and_epoll_answer_the_go_netpoll_startup' "$LRS" \
    && ok "the sidecar's netpoll startup unit test exists" \
    || bad "the sidecar's netpoll startup unit test is missing"

# ─── The six TG-1 rows: an open gap, or landed with its §5.1 class ────────
# Presence is not the failure. A table row is the dispatcher's whole answer,
# and §5.1 fixes which side owns each number, so a row that lands under a
# different class is the reviewable error: clone (56) is LSC_THREAD, futex
# (202) is LSC_FUTEX, gettid/getpid/exit (186/39/60) are LSC_TASK, and the
# measured openat path (257) is LSC_FD.
check_tg1_row() {
    local num="$1" cls="$2" line
    line="$(grep -E "^[[:space:]]*\{[[:space:]]*$num," "$TABLE" || true)"
    if [ -z "$line" ]; then
        ok "TG-1 row $num is still an open gap (not landed yet)"
    elif printf '%s\n' "$line" | grep -Eq "^[[:space:]]*\{[[:space:]]*$num,[[:space:]]*$cls([,[:space:]]|$)"; then
        ok "TG-1 row $num has landed with its §5.1 class $cls"
    else
        bad "TG-1 row $num is present with the wrong class; §5.1 requires $cls"
    fi
}
check_tg1_row 56 LSC_THREAD
check_tg1_row 202 LSC_FUTEX
check_tg1_row 186 LSC_TASK
check_tg1_row 39 LSC_TASK
check_tg1_row 60 LSC_TASK
check_tg1_row 257 LSC_FD
if grep -q 'tools/go_thread_gate_args_census.txt' "$DOC" && \
   grep -q 'tools/go_thread_gate_census.sh' "$DOC" && \
   grep -q 'tests/go_thread_gate_args_check.sh' "$DOC"; then
    ok "the gate document points to the argument census, generator, and guard"
else
    bad "the gate document no longer points to the census, generator, and guard"
fi

printf 'go_thread_gate_args_check: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

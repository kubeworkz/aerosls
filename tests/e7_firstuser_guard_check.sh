#!/usr/bin/env bash
# tests/e7_firstuser_guard_check.sh — live proof that the REAL firstuser's
# Rust std guard-page call reaches the E7 mprotect seam and SUCCEEDS.
#
# Why this exists. The real firstuser's stack-overflow setup first got ENOSYS
# from `sigaltstack(131)` (the sidecar had no arm), and after that arm landed a
# second wall appeared: the asm number chain routed 131 to the SLS svc_crash
# handler before do_syscall's personality branch. Either way Rust std's old
# `stack_t` stayed zeroed, `make_handler` returned at its `SS_DISABLE` test,
# and the task never issued `mmap(12288, PROT_READ|PROT_WRITE,
# MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK)` + `mprotect(page, 4096, PROT_NONE)`.
# The mprotect seam could therefore be green in every source/unit check while
# the real firstuser's own guard page never touched it. The boot transcript
# must show, in order:
#
#   1. `num=00000083 child=e7.linux.user.0 ok` — the sigaltstack query
#      answered, not ENOSYS;
#   2. `num=00000009 child=e7.linux.user.0 ok mmap -> 0x... len=12288 prot=3`
#      — the guard mmap is three pages and PROT_READ|PROT_WRITE before the
#      one-page carve-out, and its base is what the apply must name;
#   3. `num=0000000A child=e7.linux.user.0 ok` — the firstuser's guard-page
#      mprotect reached the sidecar and was granted (not `-> ENOSYS`);
#   4. a kernel `[E7] mprotect addr=0x... len=4096 prot=0x0 ... soft-protected
#      (P=1 U=0)` line after (3), with `addr` equal to the base from (2), its
#      one-page length exactly covering the guard page, and `prot=0x0`
#      (PROT_NONE) — the page-table apply actually ran against the page the
#      mmap reserved and left it inaccessible.
#
# A firstuser `num=00000083 ... -> ENOSYS` fails FIRST and by name, before the
# window: std then returns early, so waiting for a guard mprotect that cannot
# come would report the symptom instead of the cause. The same is true one
# step later for `num=0000000A ... -> ENOSYS`: the guard mmap happened, but
# the page never transitioned.
#
# The probe and candidate also soft-protect pages, so their lines are not
# evidence for THIS claim: the child name and the ordering above are.
#
# Usage:
#   bash tests/e7_firstuser_guard_check.sh               # boot and assert
#   bash tests/e7_firstuser_guard_check.sh --replay LOG  # assert a transcript
#
# The replay arm applies the SAME assertions without QEMU; it is what
# tests/e7_firstuser_guard_check_smoke.sh mutates (ENOSYS, missing-apply,
# out-of-order, wrong-address, short-length, missing-prot, writable-prot,
# missing-claim, missing-len, malformed-addr, missing-mmap,
# malformed-mmap-base, missing-grant, missing-sigaltstack, foreign-apply,
# mmap-short, mmap-readonly, mmap-len-missing, or mmap-prot-missing
# transcripts must fail by name, so the assertions cannot rot into "any
# transcript passes").
#
# Prerequisite: sls_operating_system.iso (make x86-iso) and
# qemu-system-x86_64 for the boot arm. Override with E7_FIRSTUSER_ISO /
# E7_FIRSTUSER_QEMU.
# GUARD-KIND: runtime (the boot arm needs the built ISO + QEMU; --replay needs
# only a transcript and is used by the source-only mutation test).
#
# Exit: 0 if the ordered firstuser guard call is present and successful,
# 1 if QEMU died first or an assertion failed, 2 on a missing prerequisite.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

REPLAY=""
case "${1:-}" in
    "")
        ;;
    --replay)
        REPLAY="${2:-}"
        if [ -z "$REPLAY" ]; then
            echo "ABORT: --replay needs a log path" >&2
            exit 2
        fi
        shift 2
        ;;
    *)
        echo "ABORT: unknown argument '$1' (usage: $0 [--replay LOG])" >&2
        exit 2
        ;;
esac
if [ "$#" -ne 0 ]; then
    echo "ABORT: unexpected argument '$1' (usage: $0 [--replay LOG])" >&2
    exit 2
fi

ISO="${E7_FIRSTUSER_ISO:-sls_operating_system.iso}"
QEMU="${E7_FIRSTUSER_QEMU:-qemu-system-x86_64}"
LOG="${E7_FIRSTUSER_LOG:-/tmp/aerosls_e7_firstuser_guard.log}"
WINDOW_S=120

booted=0
qemu_alive=1

if [ -n "$REPLAY" ]; then
    LOG="$REPLAY"
    [ -f "$LOG" ] || {
        echo "ABORT: replay log $LOG missing" >&2
        exit 2
    }
    booted=1
else
    [ -f "$ISO" ] || {
        echo "ABORT: $ISO missing — build it with: make x86-iso" >&2
        exit 2
    }
    command -v "${QEMU%% *}" >/dev/null 2>&1 || {
        echo "ABORT: ${QEMU%% *} not installed" >&2
        exit 2
    }

    rm -f "$LOG"
    $QEMU -cdrom "$ISO" -display none -m 4G -no-reboot \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
        -serial file:"$LOG" 2>/dev/null &
    QPID=$!

    for _i in $(seq 1 $((WINDOW_S / 2))); do
        # ENOSYS is terminal: std returns before the guard mmap+mprotect, so
        # stop here and report the cause rather than the missing symptom.
        if grep -aqE 'num=00000083 child=e7\.linux\.user\.0 -> ENOSYS' \
                "$LOG" 2>/dev/null; then
            break
        fi
        # 131 answered means std reaches its guard mprotect; an ENOSYS THERE
        # is terminal too, and waiting for the grant would report the symptom.
        if grep -aqE 'num=0000000A child=e7\.linux\.user\.0 -> ENOSYS' \
                "$LOG" 2>/dev/null; then
            break
        fi
        if grep -aqE 'num=0000000A child=e7\.linux\.user\.0 ok' \
                "$LOG" 2>/dev/null; then
            # The sidecar logs its grant before sending the reply; give the
            # kernel's apply one scheduling turn to print the soft-protect line.
            sleep 1
            booted=1
            break
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            qemu_alive=0
            break
        fi
        sleep 2
    done
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
fi

if grep -aqE 'num=00000083 child=e7\.linux\.user\.0 -> ENOSYS' "$LOG" 2>/dev/null; then
    echo "FAIL  firstuser sigaltstack took the ENOSYS path — Rust std returns before its guard-page mmap+mprotect" >&2
    echo "      firstuser guard lines:" >&2
    grep -aE 'num=00000083 child=e7\.linux\.user\.0|num=0000000A child=e7\.linux\.user\.0|\[E7\] mprotect' "$LOG" | tail -20 >&2
    echo "      full log: $LOG" >&2
    exit 1
fi

if grep -aqE 'num=0000000A child=e7\.linux\.user\.0 -> ENOSYS' "$LOG" 2>/dev/null; then
    echo "FAIL  firstuser mprotect took the ENOSYS path — the guard page never transitioned" >&2
    echo "      firstuser guard lines:" >&2
    grep -aE 'num=00000083 child=e7\.linux\.user\.0|num=0000000A child=e7\.linux\.user\.0|\[E7\] mprotect' "$LOG" | tail -20 >&2
    echo "      full log: $LOG" >&2
    exit 1
fi

if [ "$booted" -eq 0 ]; then
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAIL  QEMU exited before the firstuser guard mprotect appeared" >&2
    else
        echo "FAIL  within ${WINDOW_S}s the firstuser guard mprotect never appeared" >&2
    fi
    echo "      last 60 serial lines:" >&2
    tail -60 "$LOG" >&2
    exit 1
fi

fail=0

# The query must precede the mprotect, and the mprotect must not be ENOSYS.
# This awk is the ordered assertion: a probe/candidate mprotect, or a
# sigaltstack success from another child, cannot satisfy it.
awk '
    /num=00000083 child=e7\.linux\.user\.0 ok/ { sig = 1 }
    /num=0000000A child=e7\.linux\.user\.0/ {
        if (!sig) bad_order = 1;
        mprot = 1;
    }
    /num=0000000A child=e7\.linux\.user\.0 -> ENOSYS/ { enosys = 1 }
    END {
        if (!sig) { printf "FAIL  missing firstuser sigaltstack query\n"; exit 1 }
        if (!mprot) { printf "FAIL  missing firstuser mprotect grant\n"; exit 1 }
        if (bad_order) { printf "FAIL  firstuser mprotect preceded its sigaltstack query\n"; exit 1 }
        if (enosys) { printf "FAIL  firstuser mprotect still took the ENOSYS path\n"; exit 1 }
        exit 0
    }
' "$LOG" || fail=1

# The kernel's apply line must follow the sidecar's firstuser grant: this is
# what separates a granted request from a page-table change that happened.
if ! awk '
    /num=0000000A child=e7\.linux\.user\.0 ok/ { m = 1 }
    m && /\[E7\] mprotect .*prot=0x0: .*soft-protected \(P=1 U=0\)/ { applied = 1 }
    END { exit !applied }
' "$LOG"; then
    echo "FAIL  no kernel soft-protected line followed the firstuser mprotect grant" >&2
    fail=1
fi

# The apply must name the page the guard mmap reserved, cover exactly that
# one page, and leave it PROT_NONE. The child name anchors the mmap; the
# apply line has no child field, so it is the first `[E7] mprotect addr=`
# line after the firstuser grant. Hex is compared numerically: the kernel
# prints the mmap base zero-padded and the mprotect address unpadded, so a
# string compare would reject a correct boot. A malformed `addr=` or mmap
# base value is captured and rejected as unparseable rather than being
# mistaken for a missing line. Serial lines end with CRLF; a trailing CR is
# stripped from the captured tokens before parsing so a correct base is not
# rejected as unparseable.
if ! awk '
    BEGIN { GUARD_PAGE = 4096; CR = sprintf("%c", 13) }
    function stripcr(s) {
        while (substr(s, length(s)) == CR)
            s = substr(s, 1, length(s) - 1)
        return s
    }
    function hexdec(s,    i, c, v, n) {
        sub(/^0[xX]/, "", s)
        n = length(s)
        v = 0
        for (i = 1; i <= n; i++) {
            c = index("0123456789abcdef", tolower(substr(s, i, 1)))
            if (c == 0) return -1
            v = v * 16 + (c - 1)
        }
        return v
    }
    /num=00000009 child=e7\.linux\.user\.0 ok mmap -> / {
        if (!granted) {
            if (match($0, /0x[^ \t]+/))
                mmap = stripcr(substr($0, RSTART, RLENGTH))
            if (match($0, /len=[0-9]+/))
                mmap_len = substr($0, RSTART + 4, RLENGTH - 4)
            if (match($0, /prot=[0-9]+/))
                mmap_prot = substr($0, RSTART + 5, RLENGTH - 5)
        }
    }
    /num=0000000A child=e7\.linux\.user\.0 ok/ { granted = 1 }
    granted && !have_addr && /\[E7\] mprotect addr=/ {
        if (match($0, /addr=[^ \t]+/)) {
            addr = stripcr(substr($0, RSTART + 5, RLENGTH - 5))
            have_addr = 1
        }
        if (match($0, /len=[0-9]+/))
            len = substr($0, RSTART + 4, RLENGTH - 4)
        if (match($0, /prot=0x[0-9a-fA-F]+/))
            prot = substr($0, RSTART + 5, RLENGTH - 5)
    }
    END {
        if (mmap == "" || addr == "") {
            printf "FAIL  firstuser guard address missing (mmap=%s addr=%s)\n", (mmap == "" ? "none" : mmap), (addr == "" ? "none" : addr)
            exit 1
        }
        a = hexdec(mmap)
        b = hexdec(addr)
        if (a < 0 || b < 0) {
            printf "FAIL  firstuser guard address unparseable (mmap=%s addr=%s)\n", mmap, addr
            exit 1
        }
        if (a != b) {
            printf "FAIL  firstuser mprotect address mismatch: addr %s != guard mmap base %s\n", addr, mmap
            exit 1
        }
        if (mmap_len == "") {
            printf "FAIL  firstuser guard mmap length missing (mmap has no len=...)\n"
            exit 1
        }
        if (mmap_len + 0 != 3 * GUARD_PAGE) {
            printf "FAIL  firstuser guard mmap length %s is not three pages (%d)\n", mmap_len, 3 * GUARD_PAGE
            exit 1
        }
        if (mmap_prot == "") {
            printf "FAIL  firstuser guard mmap protection missing (mmap has no prot=...)\n"
            exit 1
        }
        if (mmap_prot + 0 != 3) {
            printf "FAIL  firstuser guard mmap protection %s is not PROT_READ|PROT_WRITE (3)\n", mmap_prot
            exit 1
        }
        if (len == "") {
            printf "FAIL  firstuser guard mprotect length missing (apply has no len=...)\n"
            exit 1
        }
        if (len + 0 != GUARD_PAGE) {
            printf "FAIL  firstuser guard mprotect length %s does not cover exactly one page (%d)\n", len, GUARD_PAGE
            exit 1
        }
        if (prot == "") {
            printf "FAIL  firstuser guard mprotect protection missing (apply has no prot=...)\n"
            exit 1
        }
        if (hexdec(prot) != 0) {
            printf "FAIL  firstuser guard mprotect protection %s is not PROT_NONE\n", prot
            exit 1
        }
        exit 0
    }
' "$LOG"; then
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "      firstuser guard lines:" >&2
    grep -aE 'num=00000083 child=e7\.linux\.user\.0|num=0000000A child=e7\.linux\.user\.0|\[E7\] mprotect' "$LOG" | tail -20 >&2
    echo "      full log: $LOG" >&2
    exit 1
fi

echo "PASS  firstuser guard: sigaltstack answered, guard mmap three pages RW, mprotect granted at its base with a one-page length and PROT_NONE (not ENOSYS), kernel applied it"
echo "      log: $LOG"

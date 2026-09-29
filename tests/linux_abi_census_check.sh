#!/usr/bin/env bash
# linux_abi_census_check.sh — the E7 syscall census, and its vDSO proof, gated.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# E7's dispatch table is built from tools/linux_syscall_census.txt, so that file
# is a SPECIFICATION rather than a report: an artifact that drifts from the
# binary it describes builds a shim for a binary nobody has. The census tool
# checks that itself, and it also measures the one entry a trace structurally
# cannot produce — the declared vDSO backstop — by re-running the candidate with
# no AT_SYSINFO_EHDR and comparing the two row sets
# (tools/linux_syscall_census_novdso.txt).
#
# Those teeth live in tools/linux_syscall_census.sh, which was deliberately not a
# guard while it had nothing to check: its `--run` and `--check` modes both
# refused to proceed until a candidate was DESIGNATED, and "no first user has
# been designated yet" has no honest GUARD-KIND — the missing input was a
# business decision. That is no longer the state of the tree: the backup
# operation is designated, the census is measured, and the proof is checked in.
# So the check belongs in the guard set, where run_checks.sh and deploy.sh's
# gate run it on every push.
#
# ─── What this adds to the tool's own `--check` ────────────────────────────
# Two things the tool cannot assert about itself:
#
#   1. VACUITY. The tool exiting 0 proves nothing on its own if it did not get
#      far enough to print its conclusions — a check that silently stopped
#      checking would still exit 0. So a pass here requires BOTH "census: ok"
#      and "proof: ok" in the output. (Precedent: commands_doc_check.sh, where a
#      missing python3 made every capture empty and every check report "ok".)
#
#   2. A BROKEN INSTRUMENT IS NOT A MISSING PREREQUISITE. Exit 2 means "cannot
#      check" and the runner treats it as a skip (or a failure under
#      --require-all). But bash itself exits 2 for a SYNTAX ERROR, so a census
#      tool with a typo in it would present as an owed skip and CI would stay
#      green — the silent-rot failure mode this repo has paid for more than
#      once. The tool's own aborts say "ABORT:"; anything else exiting 2 is a
#      FAILURE here.
#
# ─── What runs where ──────────────────────────────────────────────────────
# This guard is SOURCE-ONLY: it reads three checked-in files (the candidate
# designation, the census and the proof) plus the kernel's own syscall-number
# header, and needs no strace, no cargo, no built binary. That is deliberate.
# The two modes that MEASURE need a Linux host with strace and the musl target,
# so they are run by hand on a census host and CI CHECKS the result; the
# alternative — a CI step that ran `--prove-vdso` — would either need a second
# toolchain on the runner or quietly stop measuring.
#
# No GUARD-KIND marker, on purpose: as written there is nothing this guard can
# legitimately be unable to do on a Linux runner, so a skip must not be
# available to it.
#
# Exit: 0 pass, 1 the census or its proof is wrong, 2 a prerequisite is missing.
set -u
cd "$(dirname "$0")/.." || exit 1

TOOL=tools/linux_syscall_census.sh

[ -f "$TOOL" ] || {
    echo "ABORT: $TOOL is missing — the census instrument is where the teeth live." >&2
    exit 2
}

out="$(bash "$TOOL" --check 2>&1)"
rc=$?

case "$rc" in
    0)
        # The vacuity control: a pass has to have said what it passed. Both
        # conclusions are required, because either one alone is achievable by a
        # tool that stopped doing the other half of its job.
        missing=""
        printf '%s' "$out" | grep -q 'census: ok' || missing="census"
        printf '%s' "$out" | grep -q 'proof: ok'  || missing="$missing proof"
        if [ -n "$missing" ]; then
            echo "FAIL: $TOOL --check exited 0 without reporting: $missing"
            echo "      A pass that did not reach its own conclusions is not a pass."
            printf '%s\n' "$out" | sed 's/^/      /'
            exit 1
        fi
        printf '%s\n' "$out" | sed 's/^/      /'
        echo "ok:   the census still describes the designated binary, and every declared vDSO backstop is proved"
        exit 0
        ;;
    1)
        echo "FAIL: the E7 census does not check out:"
        printf '%s\n' "$out" | sed 's/^/      /'
        exit 1
        ;;
    2)
        if printf '%s' "$out" | grep -q '^ABORT:'; then
            printf '%s\n' "$out" | sed 's/^/      /'
            echo "ABORT: the census tool cannot check from here — a prerequisite is missing, and that is not a pass." >&2
            exit 2
        fi
        echo "FAIL: $TOOL exited 2 with no ABORT: line in its output."
        echo "      Two means \"cannot check\", and bash also exits 2 for a syntax error, so this"
        echo "      is reported as a FAILURE rather than an owed skip — a broken instrument"
        echo "      must not be able to present as a missing prerequisite:"
        printf '%s\n' "$out" | sed 's/^/      /'
        exit 1
        ;;
    *)
        echo "FAIL: $TOOL --check exited $rc:"
        printf '%s\n' "$out" | sed 's/^/      /'
        exit 1
        ;;
esac

#!/usr/bin/env bash
# tests/e7_firstuser_guard_check_smoke.sh — mutation teeth for
# tests/e7_firstuser_guard_check.sh.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The live check passes on the committed transcript. A check that only ever
# sees the good transcript can rot into one that accepts an ENOSYS answer,
# which is exactly the failure it exists to catch: when `sigaltstack(131)`
# returns ENOSYS, Rust std returns before its guard-page mmap+mprotect, and
# the missing mprotect is the SYMPTOM. The check must name the CAUSE and fail
# fast, not wait out the boot window for a call that can never come.
#
# ─── The arms ──────────────────────────────────────────────────────────────
#   baseline  the committed evidence transcript must replay PASS (the vacuity
#             control: a check that fails its own evidence proves nothing);
#   crlf      the same transcript serialized with CRLF, as the live serial log
#             is, must still PASS — this pins the trailing-CR strip that the
#             live boot needed;
#   enosys    the same transcript with the firstuser's 131 line changed to
#             `-> ENOSYS`, its second 131 install, its guard mmap and its
#             guard mprotect removed — exactly what the boot produces when the
#             sidecar answers ENOSYS. The check must exit 1 and print
#             `FAIL  firstuser sigaltstack took the ENOSYS path`;
#   mprotect-enosys
#             the transcript where 131 is answered and the guard mmap happens,
#             but the firstuser's mprotect(0xA) line is `-> ENOSYS` and no
#             soft-protect line follows. The check must exit 1 and print
#             `FAIL  firstuser mprotect took the ENOSYS path`;
#   soft-missing
#             mprotect(0xA) is granted, but the kernel's `soft-protected`
#             line is removed: the sidecar said yes and the page-table apply
#             never ran. The check must exit 1 and print `FAIL  no kernel
#             soft-protected line followed the firstuser mprotect grant`;
#   reordered-mprotect
#             the firstuser's mprotect(0xA) grant line is moved before its 131
#             query: every line is a success, but the causal order is wrong.
#             The ordered assertion must fail and print `FAIL  firstuser
#             mprotect preceded its sigaltstack query`;
#   wrong-addr
#             the kernel's apply line names a different page from the guard
#             mmap base; every request still succeeds, but the page protected
#             is not the page the mmap reserved. The check must exit 1 and
#             print `FAIL  firstuser mprotect address mismatch`;
#   short-len
#             the apply names the right page but `len=2048`, so it protects
#             only half of the guard page. The check must exit 1 and print
#             `FAIL  firstuser guard mprotect length ... does not cover
#             exactly one page`;
#   writable-prot
#             the apply names the right page and the full page, but asks for
#             `prot=0x2` (PROT_WRITE) instead of PROT_NONE. The check must
#             exit 1 and print `FAIL  firstuser guard mprotect protection
#             0x2 is not PROT_NONE`;
#   prot-missing
#             the apply line omits `prot=...` entirely. The address and length
#             still match, so only the missing-protection branch catches it;
#             the check must exit 1 and print `FAIL  firstuser guard mprotect
#             protection missing`;
#   no-soft-claim
#             the apply keeps addr/len/prot but drops the kernel's
#             `soft-protected (P=1 U=0)` claim. The apply-text assertion must
#             fail and print `FAIL  no kernel soft-protected line followed the
#             firstuser mprotect grant`;
#   len-missing
#             the apply line omits `len=...` entirely. The address and
#             protection still match, so only the missing-length branch
#             catches it; the check must exit 1 and print `FAIL  firstuser
#             guard mprotect length missing`;
#   malformed-addr
#             the apply carries an address with a non-hex digit. The line is
#             present, so the missing-address branch must not swallow it; the
#             check must exit 1 and print `FAIL  firstuser guard address
#             unparseable`;
#   mmap-missing
#             the firstuser's guard mmap base line is removed, so the apply
#             has no base to match. The address-missing branch must fire, not
#             a silent pass; the check must exit 1 and print `FAIL  firstuser
#             guard address missing`;
#   malformed-mmap-base
#             the mmap base line carries a non-hex digit. The line is present,
#             so the address-missing branch must not swallow it; the check
#             must exit 1 and print `FAIL  firstuser guard address
#             unparseable`;
#   grant-missing
#             the firstuser's mprotect grant line is removed. The 131 query
#             and the apply remain, but the causal request is gone; the check
#             must exit 1 and print `FAIL  missing firstuser mprotect grant`;
#   no-sigaltstack
#             both firstuser 131 lines are removed, so the grant and apply
#             remain but the query that must precede them is gone. The check
#             must exit 1 and print `FAIL  missing firstuser sigaltstack
#             query`;
#   crlf-no-sigaltstack
#             the missing-query transcript serialized with CRLF: the named
#             failure must survive both conditions together, not just each
#             one in isolation;
#   foreign-apply
#             a probe's soft-protect line follows the firstuser grant and the
#             firstuser apply is gone. The address assertion must reject the
#             foreign page, not accept it as the firstuser's; the check must
#             exit 1 and print `FAIL  firstuser mprotect address mismatch`;
#   mmap-short
#             the guard mmap asks for two pages (`len=8192`) instead of three.
#             The mmap-shape assertion must fail and print `FAIL  firstuser
#             guard mmap length`;
#   mmap-readonly
#             the guard mmap asks for `prot=1` (read-only) instead of RW. The
#             mmap-shape assertion must fail and print `FAIL  firstuser guard
#             mmap protection`;
#   mmap-len-missing
#             the granted guard mmap line omits its `len=` field entirely.
#             The checker must fail and print `FAIL  firstuser guard mmap
#             length missing`;
#   mmap-prot-missing
#             the granted guard mmap line omits its `prot=` field entirely.
#             The checker must fail and print `FAIL  firstuser guard mmap
#             protection missing`."
#
# No ISO, no QEMU: --replay applies the checker's own assertions to a
# transcript, so this smoke is source-only and runs on the source smoke sweep
# as well as on a build host.
#
# Exit: 0 if both arms behave as named, 1 otherwise, 2 on a missing
# prerequisite. A smoke that cannot run proves nothing.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

GUARD=tests/e7_firstuser_guard_check.sh
ART=evidence/e7_firstuser_guard_live.log

[ -f "$GUARD" ] || {
    echo "ABORT: $GUARD missing — the check this smoke guards is gone." >&2
    exit 2
}
[ -f "$ART" ] || {
    echo "ABORT: $ART missing — the committed transcript this smoke mutates is gone." >&2
    exit 2
}

WORK="$(mktemp -d /tmp/e7_firstuser_guard_smoke.XXXXXX)" || {
    echo "ABORT: mktemp failed." >&2
    exit 2
}
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# ─── Baseline: the committed transcript passes ─────────────────────────────
out="$(bash "$GUARD" --replay "$ART" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF 'PASS  firstuser guard:'; then
    echo "ok:   baseline — the committed transcript passes the live check"
    pass=$((pass + 1))
else
    echo "FAIL: baseline — the committed transcript no longer passes (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── CRLF: serial-line endings must not change the verdict ────────────────
# The live serial log is CRLF; the committed evidence is LF. The checker
# strips a trailing CR from captured tokens. Converting the evidence to CRLF
# must not change the verdict, or a correct real boot would be rejected.
awk '{ printf "%s\r\n", $0 }' "$ART" > "$WORK/crlf.log"

out="$(bash "$GUARD" --replay "$WORK/crlf.log" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF 'PASS  firstuser guard:'; then
    echo "ok:   CRLF — the committed transcript still passes when serialized with CRLF"
    pass=$((pass + 1))
else
    echo "FAIL: CRLF — the committed transcript failed under CRLF (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: sigaltstack(131) -> ENOSYS stops std early ───────────────────
# The rewritten transcript keeps the boot's real serial lines around the
# guard setup; only the firstuser's 131 answer and what depended on it change.
awk '
    /num=00000083 child=e7\.linux\.user\.0 ok \+ payload/ {
        print "[E7] linux: num=00000083 child=e7.linux.user.0 -> ENOSYS"
        next
    }
    /num=00000083 child=e7\.linux\.user\.0 ok$/ { next }
    /num=00000009 child=e7\.linux\.user\.0 ok mmap -> 0x00004000000e8000/ { next }
    /num=0000000A child=e7\.linux\.user\.0 ok/ { next }
    /\[E7\] mprotect addr=0x4000000e8000 .*soft-protected/ { next }
    { print }
' "$ART" > "$WORK/sigaltstack-enosys.log"

out="$(bash "$GUARD" --replay "$WORK/sigaltstack-enosys.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser sigaltstack took the ENOSYS path'; then
    echo "ok:   mutation — a firstuser 131 ENOSYS transcript fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the 131 ENOSYS transcript was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: mprotect(10) -> ENOSYS after a successful 131 ───────────────
# The guard mmap stays: std reached the mprotect, and the kernel had nothing
# to apply, so the sidecar's grant and the kernel's soft-protect line are gone.
awk '
    /num=0000000A child=e7\.linux\.user\.0 ok/ {
        print "[E7] linux: num=0000000A child=e7.linux.user.0 -> ENOSYS"
        next
    }
    /\[E7\] mprotect addr=0x4000000e8000 .*soft-protected/ { next }
    { print }
' "$ART" > "$WORK/mprotect-enosys.log"

out="$(bash "$GUARD" --replay "$WORK/mprotect-enosys.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser mprotect took the ENOSYS path'; then
    echo "ok:   mutation — a firstuser mprotect ENOSYS transcript fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the mprotect ENOSYS transcript was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: mprotect granted, kernel apply line missing ────────────────
# The sidecar's grant stays, so the request crossed and was answered; only
# the kernel's page-table apply line is gone. A check that treated the grant
# as the result would stay green while the page never changed.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 .*soft-protected/ { next }
    { print }
' "$ART" > "$WORK/soft-apply-missing.log"

out="$(bash "$GUARD" --replay "$WORK/soft-apply-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  no kernel soft-protected line followed the firstuser mprotect grant'; then
    echo "ok:   mutation — a granted mprotect with no kernel apply line fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing soft-protect apply line was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: mprotect grant moved before the 131 query ───────────────────
# Every line stays a success; only the order is wrong. The ordered assertion
# must notice, because success without the preceding ss_flags=SS_DISABLE
# answer is not the causal chain this claim is about.
# The grant appears after the query in the real transcript, so read it first:
# a single forward pass cannot move a later line earlier.
mprot_line="$(awk '
    /^\[E7\] linux: num=0000000A child=e7\.linux\.user\.0 ok$/ { print; exit }
' "$ART")"
if [ -z "$mprot_line" ]; then
    echo "ABORT: the transcript has no firstuser mprotect grant line to move." >&2
    exit 2
fi

awk -v mprot="$mprot_line" '
    $0 == mprot { next }
    /^\[E7\] linux: num=00000083 child=e7\.linux\.user\.0 ok \+ payload$/ && !placed {
        print mprot
        placed = 1
    }
    { print }
' "$ART" > "$WORK/mprotect-before-sig.log"

if ! awk '
    /num=0000000A child=e7\.linux\.user\.0 ok/ { m = NR }
    /num=00000083 child=e7\.linux\.user\.0 ok \+ payload/ { s = NR }
    END { exit !(m && s && m < s) }
' "$WORK/mprotect-before-sig.log"; then
    echo "ABORT: could not build the reordered transcript (mprotect line not before 131)." >&2
    exit 2
fi

out="$(bash "$GUARD" --replay "$WORK/mprotect-before-sig.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser mprotect preceded its sigaltstack query'; then
    echo "ok:   mutation — a mprotect before its 131 query fails the ordered assertion by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the reordered transcript was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply names the wrong page ─────────────────────────────────
# Every request succeeds and the apply line is present; only its address is
# changed to the next page. A check that only asks for "a soft-protect line
# after the grant" would stay green while the guard page is untouched.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 / {
        sub(/addr=0x4000000e8000/, "addr=0x4000000e9000")
    }
    { print }
' "$ART" > "$WORK/wrong-guard-addr.log"

out="$(bash "$GUARD" --replay "$WORK/wrong-guard-addr.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser mprotect address mismatch'; then
    echo "ok:   mutation — an apply against the wrong page fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the wrong-address apply was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply covers only half the guard page ──────────────────────
# The address still matches, so the page is the right one; the length is
# short. A check that only compares addresses would stay green while half
# the guard page remains writable.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 len=4096 / {
        sub(/len=4096/, "len=2048")
    }
    { print }
' "$ART" > "$WORK/short-guard-len.log"

out="$(bash "$GUARD" --replay "$WORK/short-guard-len.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mprotect length'; then
    echo "ok:   mutation — a half-page guard mprotect fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the short guard length was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: guard page requested writable ───────────────────────────────
# The address and length are right, so the request targets the guard page;
# only its protection is wrong. A check that only verified placement would
# stay green while the guard page is writable.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 len=4096 prot=0x0:/ {
        sub(/prot=0x0/, "prot=0x2")
    }
    { print }
' "$ART" > "$WORK/writable-guard.log"

out="$(bash "$GUARD" --replay "$WORK/writable-guard.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mprotect protection'; then
    echo "ok:   mutation — a writable guard page fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the writable guard page was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply omits its protection field ───────────────────────────
# The address and length still match, so the line is the right guard page with
# the right size; only the protection evidence is gone. A check that read
# `prot` without testing that it exists would treat an empty string as zero.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 len=4096 prot=0x0:/ {
        sub(/ prot=0x0:/, ":")
    }
    { print }
' "$ART" > "$WORK/prot-missing.log"

out="$(bash "$GUARD" --replay "$WORK/prot-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mprotect protection missing'; then
    echo "ok:   mutation — an apply omitting prot fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing protection field was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply line drops the kernel's protection claim ─────────────
# The request fields (addr, len, prot) are still correct, so the address and
# protection assertions pass; only the kernel's `soft-protected (P=1 U=0)`
# evidence is gone. A check that trusted the grant without the apply text
# would stay green.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 len=4096 prot=0x0:/ {
        sub(/soft-protected \(P=1 U=0\)/, "applied")
    }
    { print }
' "$ART" > "$WORK/no-soft-claim.log"

out="$(bash "$GUARD" --replay "$WORK/no-soft-claim.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  no kernel soft-protected line followed the firstuser mprotect grant'; then
    echo "ok:   mutation — an apply without the soft-protected claim fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing apply claim was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply omits its length field ───────────────────────────────
# The page and protection are right; only the extent is gone. A check that
# read `len` without testing that it exists would treat an empty string as
# zero and accept a protection that covers nothing.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 len=4096 prot=0x0:/ {
        sub(/ len=4096/, "")
    }
    { print }
' "$ART" > "$WORK/len-missing.log"

out="$(bash "$GUARD" --replay "$WORK/len-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mprotect length missing'; then
    echo "ok:   mutation — an apply omitting len fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing length field was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: apply carries a malformed address ──────────────────────────
# The line and its request fields are present, so this is not a missing
# apply; the address simply cannot be parsed. The check must say unparseable
# rather than silently missing or matching.
awk '
    /\[E7\] mprotect addr=0x4000000e8000 / {
        sub(/addr=0x4000000e8000/, "addr=0x4000000e80GG")
    }
    { print }
' "$ART" > "$WORK/malformed-addr.log"

out="$(bash "$GUARD" --replay "$WORK/malformed-addr.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard address unparseable'; then
    echo "ok:   mutation — a malformed apply address fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the malformed address was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: the guard mmap base line is removed ────────────────────────
# The grant and apply are intact, so the guard call still looks successful;
# only the base the apply must match is gone. A check that skipped the base
# when absent would accept an apply against an unknown page.
awk '
    /^\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> / { next }
    { print }
' "$ART" > "$WORK/mmap-missing.log"

out="$(bash "$GUARD" --replay "$WORK/mmap-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard address missing'; then
    echo "ok:   mutation — a missing guard mmap base fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing mmap base was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: mmap base is malformed ────────────────────────────────────
# The base line is present, so this is not a missing mmap; the base simply
# cannot be parsed. The check must say unparseable rather than missing.
awk '
    /^\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> 0x00004000000e8000/ {
        sub(/0x00004000000e8000/, "0x00004000000e80GG")
    }
    { print }
' "$ART" > "$WORK/malformed-mmap.log"

out="$(bash "$GUARD" --replay "$WORK/malformed-mmap.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard address unparseable'; then
    echo "ok:   mutation — a malformed mmap base fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the malformed mmap base was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: the firstuser grant line is removed ────────────────────────
# The 131 query and the kernel apply remain, so the transcript still looks
# active; only the request that should precede the apply is gone. A check
# that only looked for the apply would accept a mprotect never granted.
awk '
    /^\[E7\] linux: num=0000000A child=e7\.linux\.user\.0 ok$/ { next }
    { print }
' "$ART" > "$WORK/grant-missing.log"

out="$(bash "$GUARD" --replay "$WORK/grant-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  missing firstuser mprotect grant'; then
    echo "ok:   mutation — a missing mprotect grant fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing grant was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: both firstuser sigaltstack lines are removed ───────────────
# The grant and apply remain, so the guard call still looks successful; only
# the query that must answer before them is gone. A check that only looked
# for the later calls would accept a guard setup with no sigaltstack query.
awk '
    /num=00000083 child=e7\.linux\.user\.0 ok/ { next }
    { print }
' "$ART" > "$WORK/no-sigaltstack.log"

out="$(bash "$GUARD" --replay "$WORK/no-sigaltstack.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  missing firstuser sigaltstack query'; then
    echo "ok:   mutation — a missing sigaltstack query fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing sigaltstack query was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: CRLF + missing sigaltstack query ───────────────────────────
# The live serial log is CRLF; the missing-query failure must survive that
# serialization too, not just the happy path.
awk '
    /num=00000083 child=e7\.linux\.user\.0 ok/ { next }
    { printf "%s\r\n", $0 }
' "$ART" > "$WORK/crlf-no-sigaltstack.log"

out="$(bash "$GUARD" --replay "$WORK/crlf-no-sigaltstack.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  missing firstuser sigaltstack query'; then
    echo "ok:   mutation — the missing-sigaltstack failure survives CRLF"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the missing-sigaltstack failure did not survive CRLF (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: a probe apply follows the firstuser grant ──────────────────
# The grant anchor and a soft-protect line are present, so a check that only
# looked for "an apply after the grant" would pass. The page is a probe's,
# not the firstuser's, so the address assertion must reject it.
awk '
    /^\[E7\] linux: num=0000000A child=e7\.linux\.user\.0 ok$/ {
        print
        print "[E7] mprotect addr=0x400000007000 len=4096 prot=0x0: 1 page(s) soft-protected (P=1 U=0)"
        next
    }
    /^\[E7\] mprotect addr=0x4000000e8000 / { next }
    { print }
' "$ART" > "$WORK/foreign-apply.log"

out="$(bash "$GUARD" --replay "$WORK/foreign-apply.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser mprotect address mismatch'; then
    echo "ok:   mutation — a foreign soft-protect line fails the address assertion"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the foreign apply was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: guard mmap is only two pages ───────────────────────────────
# The base and the carve-out are unchanged; the mapping is too short, so the
# guard page is not inside the three-page RW block the allocator asked for.
awk '
    /\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> / {
        sub(/len=12288/, "len=8192")
    }
    { print }
' "$ART" > "$WORK/mmap-short.log"

out="$(bash "$GUARD" --replay "$WORK/mmap-short.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mmap length'; then
    echo "ok:   mutation — a two-page guard mmap fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the short guard mmap was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: guard mmap is read-only ────────────────────────────────────
# The base and length are unchanged; the mapping is not writable, so the
# carve-out would leave no writable stack pages around the guard.
awk '
    /\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> / {
        sub(/prot=3/, "prot=1")
    }
    { print }
' "$ART" > "$WORK/mmap-readonly.log"

out="$(bash "$GUARD" --replay "$WORK/mmap-readonly.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mmap protection'; then
    echo "ok:   mutation — a read-only guard mmap fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the read-only guard mmap was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: granted mmap line omits len ────────────────────────────────
# The address and prot remain; the checker's mmap-length capture must see
# nothing and name the missing field rather than accepting the line.
awk '
    /\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> / {
        sub(/ len=12288/, "")
    }
    { print }
' "$ART" > "$WORK/mmap-len-missing.log"

out="$(bash "$GUARD" --replay "$WORK/mmap-len-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mmap length missing'; then
    echo "ok:   mutation — a granted mmap without len fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the len-missing grant was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

# ─── Mutation: granted mmap line omits prot ───────────────────────────────
# The address and len remain; the checker's mmap-protection capture must see
# nothing and name the missing field rather than accepting the line.
awk '
    /\[E7\] linux: num=00000009 child=e7\.linux\.user\.0 ok mmap -> / {
        sub(/ prot=3/, "")
    }
    { print }
' "$ART" > "$WORK/mmap-prot-missing.log"

out="$(bash "$GUARD" --replay "$WORK/mmap-prot-missing.log" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF 'FAIL  firstuser guard mmap protection missing'; then
    echo "ok:   mutation — a granted mmap without prot fails by name"
    pass=$((pass + 1))
else
    echo "FAIL: mutation — the prot-missing grant was not rejected by name (exit $rc)"
    printf '%s\n' "$out" | tail -n 10 | sed 's/^/      /'
    fail=$((fail + 1))
fi

echo
echo "e7_firstuser_guard_check_smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

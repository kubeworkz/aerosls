#!/usr/bin/env bash
# tests/linux_abi_census_check_smoke.sh — proves the census guard's teeth bite.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/linux_abi_census_check.sh asserts that the E7 census still describes the
# designated binary and that every declared vDSO backstop is proved by a
# no-vDSO re-run of the same candidate. Its teeth were proven once, by hand, in
# one worktree. Nothing in CI ever makes it fail, so a regression that made it
# blind — a dropped proof requirement, a row comparison that stopped comparing,
# a `--check` that exited 0 without checking — would pass every push unnoticed.
#
# This smoke plants four violations and asserts the guard exits 1 naming each
# one. They are deliberately of four different KINDS, because the guard has four
# separable jobs:
#
#   1. `vdso-proved`   — the proof states the syscall was proved. Removing that
#                        line must redden: a declaration with no measurement is
#                        the inference this whole instrument exists to replace.
#   2. `proof-rows`    — the proof's row set matches the census. Deleting one
#                        ordinary row must redden: if two runs of the same
#                        candidate disagree about anything besides the declared
#                        syscalls, one of them measured something else.
#   3. `declared`      — the declaration is not self-contradictory. Declaring a
#                        syscall the census traced is refused, because "backstopped
#                        by a vDSO the trace cannot see" and "in the trace" cannot
#                        both be true of the same call.
#   4. `instrument`    — a BROKEN INSTRUMENT IS NOT A MISSING PREREQUISITE. This
#                        is the tooth for the guard's own second job, and it
#                        exists because the failure is live, not theoretical:
#                        bash exits **2** for a syntax error, and exit 2 is the
#                        run_checks convention for "cannot check" — an owed
#                        skip. So a typo in the census tool would present as a
#                        legitimate skip and CI would stay green. The tooth
#                        appends an unclosed `if`, which really does make bash
#                        exit 2 (measured, not assumed), and requires the guard
#                        to report FAILURE rather than pass the 2 through.
#
# ─── How the teeth are planted and restored ────────────────────────────────
# Two different restore mechanisms, for a reason.
#
# The two ARTIFACTS are the specification this guard protects, so they are
# restored with `git checkout --`, which puts back the committed blob
# byte-identically. That restore is only safe because both are clean at entry,
# so the pre-check below aborts (exit 2, the run_checks convention) if either has
# been modified — the smoke can never clobber a real edit, nor an artifact a
# `--run`/`--prove-vdso` has just regenerated.
#
# The TOOL is not treated that way, deliberately: it is the code under active
# development, and requiring it to be clean would mean this smoke cannot run
# while the instrument is being edited — which is exactly when its teeth are
# most worth checking. So the tool is copied aside before the mutation and
# restored from that copy, and the restore is verified by sha256 against what was
# there at entry. That also means the tool tooth proves nothing about the tool
# being committed, which is correct: it tests the GUARD's handling of a broken
# exit code, not the tool's contents.
#
# The trap restores everything on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on the same set; this
# script dirties the worktree on purpose, so it must not run in either.
# run_source_smokes.sh globs tests/*_smoke.sh and excludes the build-needing set
# by name, so it picks this one up automatically — the failure that protects
# against is the one where a new smoke only ever runs on a build host and so
# never runs at all.
#
# Exit: 0 if every tooth bit, 1 if any did not, 2 if an artifact was dirty.
set -u
cd "$(dirname "$0")/.." || exit 1

guard=tests/linux_abi_census_check.sh
TOOL=tools/linux_syscall_census.sh
ART=tools/linux_syscall_census.txt
PROOF=tools/linux_syscall_census_novdso.txt
fails=0

[ -f "$guard" ] || { echo "ABORT: $guard missing" >&2; exit 2; }
for f in "$TOOL" "$ART" "$PROOF"; do
    [ -f "$f" ] || { echo "ABORT: $f missing — nothing to plant a tooth in" >&2; exit 2; }
done
for f in "$ART" "$PROOF"; do
    if [ -n "$(git status --porcelain "$f")" ]; then
        echo "ABORT: $f is dirty — the smoke needs a clean checkout to restore it" >&2
        exit 2
    fi
done

tool_sha="$(sha256sum "$TOOL" | cut -d' ' -f1)"
tmp="$(mktemp -d)"
cp "$TOOL" "$tmp/tool.bak"
trap 'git checkout -- "$ART" "$PROOF" 2>/dev/null || true; cp "$tmp/tool.bak" "$TOOL" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# tooth <label> <expected-substring> <mutation>
tooth() {
    local label="$1" expected="$2" mutate="$3" out rc
    if ! eval "$mutate"; then
        echo "TOOTH FAIL $label — the mutation itself failed"
        fails=$((fails + 1))
        return
    fi
    out="$(bash "$guard" 2>&1)"
    rc=$?
    git checkout -- "$ART" "$PROOF"
    if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
        echo "TOOTH OK   $label — guard failed with: $expected"
    else
        echo "TOOTH FAIL $label — guard rc=$rc, expected failure '$expected'"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
    fi
}

# 1. The proof stops saying the syscall was proved, while still containing its
#    row: the measurement is there, the statement is not.
tooth "vdso-proved" "was declared, not measured" \
    "sed -i '/^# vdso-proved: /d' '$PROOF'"

# 2. One ordinary row disappears from the proof. The declared syscall is still
#    stated and still has its row, so only the set comparison can catch it — and
#    this is what makes that comparison load-bearing rather than decorative.
tooth "proof-rows" "disagree in more than the declared" \
    "sed -i '/^0 read /d' '$PROOF'"

# 3. The declaration names a syscall the census already traced, which is the one
#    thing a declaration must never be: two claims about the same call.
tooth "declared" "BOTH as a traced syscall and as vdso-backstopped" \
    "sed -i 's/^# vdso-backstopped: 228 clock_gettime\$/# vdso-backstopped: 0 read/' '$ART'"

# 4. The instrument itself breaks. An unclosed `if` is a syntax error, bash
#    exits 2 for it — the same code as "prerequisite missing" — and the guard
#    must not let that present as an owed skip.
printf 'if true; then\n' >> "$TOOL"
out="$(bash "$guard" 2>&1)"
rc=$?
cp "$tmp/tool.bak" "$TOOL"
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "exited 2 with no ABORT: line"; then
    echo "TOOTH OK   instrument — a syntax error reddens the guard instead of skipping it"
else
    echo "TOOTH FAIL instrument — guard rc=$rc, expected 1 naming 'exited 2 with no ABORT: line'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi
if [ "$(sha256sum "$TOOL" | cut -d' ' -f1)" = "$tool_sha" ]; then
    echo "TOOL OK    the census tool is byte-identical after the instrument tooth"
else
    echo "TOOL FAIL  the census tool was NOT restored byte-identically"
    fails=$((fails + 1))
fi

# Final restore check — prove both artifacts are byte-identical again, so this
# smoke can never leave a footprint for a later step or a later local run.
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ] && printf '%s' "$out" | grep -q 'proof: ok'; then
    echo "RESTORE OK — both artifacts clean after every tooth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the artifacts were restored:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "linux_abi_census_check_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]

#!/usr/bin/env bash
# tests/env_shell_surface_check_smoke.sh — proves tests/env_shell_surface_check.sh
# has teeth: pointed at inputs that do NOT have the E6-shell property, the guard
# must FAIL (not pass quietly, and not hang), and it must fail for the RIGHT
# REASON rather than for some unrelated reason that happens to be red.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# env_shell_surface_check.sh asserts that four shell verbs reach a real
# environment. Every one of its clauses could be vacuous in a different way:
# `env list` could be checked against a listing that is allowed to be empty;
# the input-forwarding clause could pass on a guard that never sent anything;
# the post-destroy clause could pass on a guard that never destroyed anything.
# A guard whose assertions are never really evaluated looks exactly like one
# that holds, and this file is what tells them apart.
#
# Five arms, four of them teeth, plus an instant prerequisite arm:
#
#   1. PREREQUISITE — E6S_ISO pointing at nothing must exit 2 and say what to
#      build. run_checks.sh files a runtime guard's 2 as an owed skip, so a
#      guard that exited 0 here would look like it had passed.
#
#   2. TOOTH (wrong boot) — E6S_BOOT_ENTRY=2 boots grub's kernel-only entry,
#      which has the kernel HTTP server but NO unified control plane and NO
#      init, so there is no environment manager and no `env` surface to drive.
#      The guard must fail on its own contradiction check, early, rather than
#      waiting out the window on a dead premise.
#
#   3. TOOTH (no environment) — E6S_TOOTH=no-env withholds the HTTP create and
#      requires the guard to go red on the LISTING clause. This is the tooth
#      for "the listing is allowed to be empty": a guard that accepted live=0 as
#      a pass could never catch a registry that lost an environment.
#
#   4. TOOTH (no input) — E6S_TOOTH=no-input runs the real boot but withholds
#      the two `env attach … <line>` sends, so the marker can never appear and
#      the input-forwarding clause MUST fail. On this arm the listing and the
#      identity clauses must still have PASSED — asserted below — so the arm
#      reddens the forwarding clause and nothing else.
#
#   5. TOOTH (no destroy) — E6S_TOOTH=no-destroy withholds the destroy and
#      requires the post-destroy clause to go red. Backwards it is the same
#      claim: the clause reads the listing back through the shell, so it also
#      catches a destroy that answered `ok` without ending anything.
#
#   6. RESTORE — the guard unmodified on the unified boot must pass, so the
#      smoke also proves the guard is not simply failing everything (a tooth
#      that fires on both inputs proves nothing about either).
#
# The SOURCE control that matches arm 5 — making `env destroy` answer without
# calling the manager — is not automated here because it needs a second kernel
# build; it is the same shape as tests/env_create_boot_check_smoke.sh's
# recorded controls, and the guard honours E6S_ISO so it can be repeated
# against a sabotaged image without touching the shipped one.
#
# Needs the built ISO + QEMU, so it is on run_source_smokes.sh's build-needed
# exclusion list and runs on a build host via run_guard_smokes.sh (and in CI's
# kernel-guards job) — same as the other boot smokes.
#
# Exit: 0 when every tooth fired and the real input still passed, 1 otherwise,
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

[ -f sls_operating_system.iso ] || {
    echo "ABORT: sls_operating_system.iso missing — build it with: make x86-iso" >&2
    exit 2
}
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "ABORT: qemu-system-x86_64 not installed" >&2
    exit 2
}

fails=0

# arm <label> <env-assignments...> -- <needle> -- <also-needle...>
# Runs the guard under the given environment and requires rc=1 with the first
# needle present (the failure named) and every later needle present too (the
# clauses that must still have passed, so the arm is not red for a wrong
# reason). Assignments are passed as `VAR=value` argv entries.
arm() {
    local label="$1"; shift
    local envs=() needle="" also=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift                                    # the first --
    needle="$1"; shift
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do also+=("$1"); shift; done

    echo "TOOTH  $label"
    local out rc
    out="$(env "${envs[@]}" bash tests/env_shell_surface_check.sh 2>&1)"
    rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "TOOTH FAIL guard rc=$rc, expected 1" >&2
        printf '%s\n' "$out" | tail -25 | sed 's/^/           /' >&2
        fails=$((fails + 1))
        return
    fi
    if ! printf '%s' "$out" | grep -qF "$needle"; then
        echo "TOOTH FAIL guard failed, but not for the expected reason — '$needle' is absent from its report" >&2
        printf '%s\n' "$out" | tail -25 | sed 's/^/           /' >&2
        fails=$((fails + 1))
        return
    fi
    local n ok=1
    for n in ${also+"${also[@]}"}; do
        if ! printf '%s' "$out" | grep -qF "$n"; then
            echo "TOOTH FAIL the guard was also expected to keep '$n' GREEN on this arm (it did not)" >&2
            ok=0
        fi
    done
    if [ "$ok" -ne 1 ]; then
        printf '%s\n' "$out" | tail -25 | sed 's/^/           /' >&2
        fails=$((fails + 1))
        return
    fi
    echo "TOOTH OK   guard failed (rc=1) on: $needle"
    printf '%s\n' "$out" | grep -F "FAILED:" | head -2 | sed 's/^/           /'
}

# ── Arm 1: a missing prerequisite is an abort, never a pass ────────────────
echo "TOOTH  env_shell_surface_check.sh with the ISO pointed at nothing"
out="$(E6S_ISO=/nonexistent/does-not-exist.iso bash tests/env_shell_surface_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -qF "ABORT"; then
    echo "TOOTH OK   guard aborted (rc=2) and said what to build"
else
    echo "TOOTH FAIL guard rc=$rc with a missing ISO, expected 2 with an ABORT line" >&2
    printf '%s\n' "$out" | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

# ── Arm 2: a boot with no environment manager must fail the guard ──────────
arm "env_shell_surface_check.sh pointed at the kernel-only boot (grub entry 2)" \
    "E6S_BOOT_ENTRY=2" "E6S_WINDOW_S=45" -- \
    "FAILED:"

# ── Arm 3: the listing clause must bite with no environment created ────────
arm "env_shell_surface_check.sh with E6S_TOOTH=no-env (no environment created)" \
    "E6S_TOOTH=no-env" -- \
    "reported no environment"

# ── Arm 4: the forwarding clause must bite with nothing sent ───────────────
arm "env_shell_surface_check.sh with E6S_TOOTH=no-input (nothing forwarded)" \
    "E6S_TOOTH=no-input" -- \
    "never produced its output" \
    "--" \
    "reports the environment" \
    "carries the environment's own announcement"

# ── Arm 5: the post-destroy clause must bite with no destroy ───────────────
arm "env_shell_surface_check.sh with E6S_TOOTH=no-destroy (nothing destroyed)" \
    "E6S_TOOTH=no-destroy" -- \
    "is still listed after" \
    "--" \
    "reports the environment"

# ── RESTORE: the real input must still pass ────────────────────────────────
echo "RESTORE env_shell_surface_check.sh on the unified boot (grub entry 3)"
out="$(bash tests/env_shell_surface_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "RESTORE OK guard passed on the unified boot"
    printf '%s\n' "$out" | grep -F "ok:" | sed 's/^/           /'
else
    echo "RESTORE FAIL guard rc=$rc on the unified boot, expected 0" >&2
    printf '%s\n' "$out" | tail -30 | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || { echo "FAIL  $fails tooth/restore assertion(s) failed" >&2; exit 1; }
echo "OK: the E6 shell-surface guard reddens on every withheld step, for the reason it names, and passes when the surface works (teeth proven)"
exit 0

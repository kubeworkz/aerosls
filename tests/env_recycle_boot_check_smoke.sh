#!/usr/bin/env bash
# tests/env_recycle_boot_check_smoke.sh — proves
# tests/env_recycle_boot_check.sh has teeth: pointed at inputs that do NOT have
# the E5 property, the guard must FAIL (not pass quietly, and not hang).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# env_recycle_boot_check.sh asserts a property of ONE boot (POSIX-Environments
# E5: N create -> destroy cycles longer than every table an environment
# occupies, with slots, registry entries, process slots and frames all coming
# back). Its assertions are counts and deltas, and a count assertion is the one
# kind that can pass for the wrong reason: a loop that silently created nothing
# leaves every delta at zero, and a guard that tolerated a drift as large as an
# environment would have passed on a tree that leaked one — which is exactly
# what the FIRST run of this guard found (`chan_create failed (-7)` at the
# twelfth environment, with the frame deltas still at zero).
#
# Four arms, three of them teeth:
#
#   1. TOOTH (wrong boot) — E5_BOOT_ENTRY=2 boots grub's kernel-only entry,
#      which has the kernel HTTP server but NO unified control plane and NO
#      init, so there is no environment manager to drive: the guard must fail,
#      and its own contradiction check names it early.
#
#   2. TOOTH (no recycle) — E5_TOOTH=skip-destroy runs the guard on the REAL
#      unified boot but withholds every destroy. This is the roadmap §8 tooth
#      ("skip the registry cleanup — the cycle that exceeds the registry size
#      fails with registry full") reproduced without a second kernel build: no
#      slot, entry or frame can come back, so the loop runs into the smallest
#      fixed table an environment occupies and a create must fail. The guard
#      must go red naming that cycle. A guard whose recycle assertion was
#      vacuous — counting creates it never checked, or tolerating a failed one
#      — passes this input, and the smoke fails it for that.
#
#   3. PREREQUISITE CONTRACT — E5_ISO pointing at nothing must exit 2 and say
#      why. run_checks.sh files a runtime guard's 2 as an owed skip, so a guard
#      that "passed" a missing ISO would be a guard nobody runs.
#
#   4. RESTORE — the guard unmodified on the unified boot must pass, so the
#      smoke also proves the guard is not simply failing everything (a tooth
#      that fires on both inputs proves nothing about either).
#
# The SOURCE control that matches arm 2 — the same deletion in kernel C that
# the roadmap names, plus the two halves of the channel leak the guard found
# (init's messenger ends and the kernel's channel-id space) — was run by hand
# against rebuilt images and is recorded in the E5 commit; the guard honours
# E5_ISO so any of them can be repeated without touching the shipped ISO.
#
# Needs the built ISO + QEMU, so it is on run_source_smokes.sh's build-needed
# exclusion list and runs on a build host via run_guard_smokes.sh (and in CI's
# kernel-guards job) — same as the E1/E4 boot smokes.
#
# Exit: 0 when every tooth fired and the real input still passed, 1 otherwise,
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E5_SMOKE_ISO:-sls_operating_system.iso}"
GUARD=tests/env_recycle_boot_check.sh
# Two cycle counts, both deliberate. The TOOTH arm shortens the loop: with
# every destroy withheld the smallest fixed table an environment occupies (the
# kernel's 16-slot process table, shared with the boot's own five processes)
# is exhausted at the sixth environment, so nine cycles is enough to see it and
# the guard must still name that cycle. The RESTORE arm runs the guard's OWN
# DEFAULT — no E5_CYCLES override at all — because that is what CI runs: the
# property is that N exceeds PROC_MAX and SIDECAR_REGISTRY_MAX (16), and a
# restore arm at nine cycles would pass on a guard whose default had been
# quietly lowered below the number that makes it mean something.
TOOTH_CYCLES="${E5_SMOKE_TOOTH_CYCLES:-9}"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD missing" >&2; exit 2; }

fail=0
run_arm() {   # run_arm <expected-verdict: pass|fail|abort> <label> <env...>
    local want="$1" label="$2"; shift 2
    local out rc
    out="$(env "$@" timeout 600 bash "$GUARD" 2>&1)"; rc=$?
    local got
    case "$rc" in
        0) got=pass ;;
        2) got=abort ;;
        *) got=fail ;;
    esac
    if [ "$got" = "$want" ]; then
        echo "ok:   $label (verdict: $got, exit $rc)"
        printf '%s\n' "$out" | grep -aE "^(ok|FAILED|FAIL|PASS|note):" | head -4 | sed 's/^/      /'
    else
        echo "FAIL: $label — wanted '$want', got '$got' (exit $rc)" >&2
        printf '%s\n' "$out" | tail -12 | sed 's/^/      /' >&2
        fail=1
    fi
}

# ── Arm 1: the prerequisite contract (no ISO needed; runs anywhere) ────────
run_arm abort "a missing ISO is refused (exit 2), never a quiet pass" \
    E5_ISO=/nonexistent/aerosls_e5_smoke.iso

if [ ! -f "$ISO" ] || ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "note: $ISO or qemu-system-x86_64 missing — the three booting arms cannot run here" >&2
    [ "$fail" -eq 0 ] && echo "note: prerequisite contract only (this is a build host smoke)"
    exit "$fail"
fi

# ── Arm 2: TOOTH — the wrong boot has no environment manager at all ────────
run_arm fail "the kernel-only boot (entry 2) fails the guard — no environment manager exists there" \
    E5_ISO="$ISO" E5_BOOT_ENTRY=2 E5_WINDOW_S=90 E5_CYCLES="$TOOTH_CYCLES"

# ── Arm 3: TOOTH — withhold the destroy, and the loop cannot recycle ───────
run_arm fail "withholding every destroy turns the guard red (nothing recycles: the fixed tables fill)" \
    E5_ISO="$ISO" E5_TOOTH=skip-destroy E5_CYCLES="$TOOTH_CYCLES"

# ── Arm 4: RESTORE — the guard unmodified, at its own default depth ───────
run_arm pass "the guard unmodified passes on the unified boot at its default cycle count (the teeth are not blanket failures)" \
    E5_ISO="$ISO"

[ "$fail" -eq 0 ] && echo "ALL PASS: env_recycle_boot_check.sh's teeth bite and the real input still passes"
exit "$fail"

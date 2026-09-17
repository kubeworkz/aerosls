#!/usr/bin/env bash
# tests/run_source_smokes.sh — runs the SOURCE-ONLY guard smokes.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# run_guard_smokes.sh globs tests/*_smoke.sh, but eight of the
# thirty-four need the linked kernel, its objects, or the built ISO
# (they synthesize tooth inputs and then restore-check against the real
# image, or boot the ISO under QEMU), so that runner only works on a
# build host. The other twenty-six are source-only: they need only
# gcc/binutils/python3/openssl/cargo/sbcl and the sources in the tree —
# nothing built. CI's verify job has no build, so those twenty-six can
# and must run there.
#
# A source-only smoke that runs ONLY on build hosts is a smoke that rots
# unseen when the build breaks: the build host's job (kernel-guards) dies
# at its build step before ever reaching its smokes, and CI stays green
# anyway. tls_cert_oracle_smoke.sh is the precedent — it failed at link
# for months, invisible until the deploy gate ran it. This runner is what
# the verify job calls, so every source-only smoke's teeth are proven on
# every push, independent of the kernel build.
#
# The eight build-needing smokes are excluded by name — five carry
# GUARD-KIND: build for run_checks.sh, and three boot the built ISO (the
# two phase5 smokes and the E1 unified-boot tooth). Everything else that
# globs *_smoke.sh runs here, so a NEW source-only smoke is covered
# automatically; and a smoke that is NOT on the exclusion list but cannot
# run is a failure, not a skip, so a smoke that stops being source-only
# cannot silently go dark.
#
# Exit: 0 if every smoke passed, 1 otherwise, 2 if no smokes were found.
set -u
cd "$(dirname "$0")/.."   # repo root

# The eight build-needing smokes — the same set as the GUARD-KIND: build
# markers in run_checks.sh, plus the boot smokes (they boot
# sls_operating_system.iso under QEMU, which only exists on a build host;
# kernel-guards runs them for real right after `make x86-iso`). Keep in
# step with what kernel-guards builds.
BUILD_SMOKES="code_buffer_budget_smoke.sh kernel_image_end_smoke.sh no_hosted_link_smoke.sh no_tls_relocations_smoke.sh stack_frame_budget_smoke.sh phase5_boot_smoke.sh phase5_e1000_driver_smoke.sh unified_boot_check_smoke.sh"

shopt -s nullglob
smokes=(tests/*_smoke.sh)
shopt -u nullglob

if [ "${#smokes[@]}" -eq 0 ]; then
    echo "ABORT: no tests/*_smoke.sh found. Run from the repo root." >&2
    exit 2
fi

pass=0
fail=0

echo "run_source_smokes"
echo "================="
echo

for s in "${smokes[@]}"; do
    name="$(basename "$s")"
    case " $BUILD_SMOKES " in
        *" $name "*) continue ;;
    esac
    out="$("bash" "$s" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name (exit $rc)"
        printf '%s\n' "$out" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done

echo
echo "$pass passed, $fail failed"

[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# tests/run_source_smokes.sh — runs the SOURCE-ONLY guard smokes and the
# source-only launcher harness.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# run_guard_smokes.sh globs tests/*_smoke.sh, but ten of the
# fifty-one need the linked kernel, its objects, or the built ISO
# (they synthesize tooth inputs and then restore-check against the real
# image, or boot the ISO under QEMU), so that runner only works on a
# build host. The other forty-one are source-only: they need only
# gcc/binutils/python3/openssl/cargo/sbcl and the sources in the tree —
# nothing built. CI's verify job has no build, so those forty-one can
# and must run there.
#
# A source-only smoke that runs ONLY on build hosts is a smoke that rots
# unseen when the build breaks: the build host's job (kernel-guards) dies
# at its build step before ever reaching its smokes, and CI stays green
# anyway. tls_cert_oracle_smoke.sh is the precedent — it failed at link
# for months, invisible until the deploy gate ran it. This runner is what
# the verify job calls, so every source-only smoke's teeth are proven on
# every push, independent of the kernel build. (aerofs_v2_check_smoke.sh is
# source-only by construction: it builds a minimal root holding exactly
# the files its guard reads, so proving its teeth
# needs neither a build nor a boot; only its vacuity control runs the guard
# against the real tree). env_storage_durable_check_smoke.sh is the most
# recent addition and follows the same shape: a minimal root holding exactly
# the ten-file include closure the guard's own host build needs (S11 builds
# inside the inspected root), and a synthesized --replay fixture set for the
# boot arm's validator — so proving its teeth needs neither a build nor a
# boot either.
#
# The ten build-needing smokes are excluded by name — five carry
# GUARD-KIND: build for run_checks.sh, and five boot the built ISO (the
# two phase5 smokes, the E1 unified-boot tooth, the E4
# environment-create tooth and the E6 shell-surface tooth). Everything else that
# globs *_smoke.sh runs here, so a NEW source-only smoke is covered
# automatically; and a smoke that is NOT on the exclusion list but cannot
# run is a failure, not a skip, so a smoke that stops being source-only
# cannot silently go dark.
#
# The launcher harness (tests/run_cluster_harness.sh) is appended
# explicitly: it is not named *_smoke.sh, but it is source-only (stub
# qemu/make/grub on PATH, no build) and drives run-cluster.sh's arms,
# including the sidecar RAM floor. The launcher must be exercised on every
# push, not only on a build host, so it is held to the same exit contract
# as the smokes.
#
# Exit: 0 if every test passed, 1 otherwise, 2 if no smokes were found.
set -u
cd "$(dirname "$0")/.."   # repo root

# The ten build-needing smokes — the same set as the GUARD-KIND: build
# markers in run_checks.sh, plus the boot smokes (they boot
# sls_operating_system.iso under QEMU, which only exists on a build host;
# kernel-guards runs them for real right after `make x86-iso`). Keep in
# step with what kernel-guards builds.
BUILD_SMOKES="code_buffer_budget_smoke.sh kernel_image_end_smoke.sh no_hosted_link_smoke.sh no_tls_relocations_smoke.sh stack_frame_budget_smoke.sh phase5_boot_smoke.sh phase5_e1000_driver_smoke.sh unified_boot_check_smoke.sh env_create_boot_check_smoke.sh env_shell_surface_check_smoke.sh"

shopt -s nullglob
smokes=(tests/*_smoke.sh)
shopt -u nullglob

# Not a *_smoke.sh by name, but source-only and part of the launcher's
# guard teeth. Appended explicitly so the glob's fail-closed rule still
# governs every smoke, and this harness is held to the same contract.
extra=(tests/run_cluster_harness.sh)

if [ "${#smokes[@]}" -eq 0 ]; then
    echo "ABORT: no tests/*_smoke.sh found. Run from the repo root." >&2
    exit 2
fi

pass=0
fail=0

echo "run_source_smokes"
echo "================="
echo

for s in "${smokes[@]}" "${extra[@]}"; do
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

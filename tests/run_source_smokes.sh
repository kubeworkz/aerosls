#!/usr/bin/env bash
# tests/run_source_smokes.sh — runs the SOURCE-ONLY guard smokes.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# run_guard_smokes.sh globs tests/*_smoke.sh, but five of the nineteen need
# the linked kernel or its objects (they synthesize tooth inputs and then
# restore-check against the real image), so that runner only works on a
# build host. The other fourteen are source-only: they need only
# gcc/binutils/python3/openssl and the sources in the tree — nothing built.
# CI's verify job has no build, so those fourteen can and must run there.
#
# A source-only smoke that runs ONLY on build hosts is a smoke that rots
# unseen when the build breaks: the build host's job (kernel-guards) dies
# at its build step before ever reaching its smokes, and CI stays green
# anyway. tls_cert_oracle_smoke.sh is the precedent — it failed at link
# for months, invisible until the deploy gate ran it. This runner is what
# the verify job calls, so every source-only smoke's teeth are proven on
# every push, independent of the kernel build.
#
# The five build-needing smokes are excluded by name — the same five
# guards carry GUARD-KIND: build for run_checks.sh. Everything else that
# globs *_smoke.sh runs here, so a NEW source-only smoke is covered
# automatically; and a smoke that is NOT on the exclusion list but cannot
# run is a failure, not a skip, so a smoke that stops being source-only
# cannot silently go dark.
#
# Exit: 0 if every smoke passed, 1 otherwise, 2 if no smokes were found.
set -u
cd "$(dirname "$0")/.."   # repo root

# The five smokes that need the linked kernel or its objects (the same set
# as the GUARD-KIND: build markers in run_checks.sh). Keep in step.
BUILD_SMOKES="code_buffer_budget_smoke.sh kernel_image_end_smoke.sh no_hosted_link_smoke.sh no_tls_relocations_smoke.sh stack_frame_budget_smoke.sh"

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

#!/usr/bin/env bash
# tests/run_guard_smokes.sh — runs every tests/*_smoke.sh teeth check.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/run_checks.sh runs every tests/*_check.sh guard; the guards' TEETH
# are proven by the tests/*_smoke.sh scripts, which deliberately break a
# guard's input and assert it fails. Thirty-two smokes are source-only (they
# need only gcc/binutils/python3/openssl and the tree's sources) and run
# anywhere -- CI runs them on every push via tests/run_source_smokes.sh.
# Five (code_buffer_budget, kernel_image_end, no_hosted_link,
# no_tls_relocations, stack_frame_budget) need a built kernel and its
# objects -- CI has no cross toolchain, so they run on a build host:
#
#     make            # or make x86-iso — build the kernel and objects
#     bash tests/run_guard_smokes.sh
#
# ─── Strictness ────────────────────────────────────────────────────────────
# A guard's *_check.sh can legitimately exit 2 when a prerequisite is
# missing, and run_checks.sh reports that as a SKIP. A *_smoke.sh has no
# such luxury: a smoke that cannot run is a smoke that proved nothing, so
# this runner counts every non-zero exit as a FAILURE and prints the
# smoke's own explanation. This is the --require-all discipline applied to
# the smokes unconditionally.
#
# Exit: 0 if every smoke passed, 1 otherwise, 2 if no smokes were found.
set -u
cd "$(dirname "$0")/.."   # repo root, so each smoke's own paths resolve

shopt -s nullglob
smokes=(tests/*_smoke.sh)
shopt -u nullglob

if [ "${#smokes[@]}" -eq 0 ]; then
    echo "ABORT: no tests/*_smoke.sh found. Run from the repo root." >&2
    exit 2
fi

pass=0
fail=0

echo "run_guard_smokes"
echo "================"
echo

for s in "${smokes[@]}"; do
    name="$(basename "$s")"
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

if [ "$fail" -gt 0 ]; then
    echo
    echo "A smoke that cannot run proves nothing. The three kernel-needing"
    echo "smokes need a build: run 'make' (or 'make x86-iso') first."
fi

[ "$fail" -eq 0 ]

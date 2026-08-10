#!/usr/bin/env bash
# makefile_sources_check.sh — every kernel .c is actually in the build.
#
# ─── The gap this closes ──────────────────────────────────────────────────
# kernel/boot_params.c shipped without being added to X86_C_SRC. It compiled
# clean, it had 49 passing host-test checks, and the whole-image link check
# passed -- because that check GLOBS the tree for .c files rather than
# reading the Makefile's list. So it happily linked a file the real build
# would never have compiled, and `make x86-iso` would have failed on
# undefined boot_params_scan_mb2().
#
# Every verification step that ran was looking somewhere the bug was not.
# The fix is not "remember to edit the Makefile"; it is a check whose whole
# job is comparing the two lists.
#
# Run:  bash tests/makefile_sources_check.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Directories whose .c files belong in the x86 kernel image.
SEARCH_DIRS="kernel net drivers arch/x86"

# Deliberate exclusions, each with a reason. Anything NOT listed here and
# not in X86_C_SRC is a finding, so this list is the only place a "we meant
# to leave that out" can hide -- and it has to say why.
declare -A EXCLUDED=(
  ["kernel/kernel_riscv.c"]="RISC-V port, not in the x86 image"
  ["kernel/kernel_arm64.c"]="AArch64 kernel main (M4a, plan doc §6/§10.187) — built by `make arm64-elf`, not the x86 image"
  ["kernel/simi_arm.c"]="AArch64 translator staging copy (M3 kernel half, plan doc §6/§10.180) — linked into the arm64 kernel by M4b; its honesty is enforced by the byte-identity re-diff + freestanding-compile gates, not by the x86 image"
)

echo "=== Makefile X86_C_SRC vs the tree ==="
echo

# The list as the build actually sees it, with line continuations joined.
IN_MAKE="$(python3 - <<'PY'
import re
src = open('Makefile').read()
m = re.search(r'^X86_C_SRC\s*=\s*((?:[^\n\\]*\\\n)*[^\n]*)', src, re.M)
if not m:
    raise SystemExit("could not find X86_C_SRC in the Makefile")
for f in m.group(1).replace('\\\n', ' ').split():
    if f.endswith('.c'):
        print(f)
PY
)" || { echo "FAIL: could not parse X86_C_SRC" >&2; exit 1; }

if [ -z "$IN_MAKE" ]; then echo "FAIL: X86_C_SRC parsed empty" >&2; exit 1; fi
echo "X86_C_SRC lists $(echo "$IN_MAKE" | wc -l) C files."

missing=0
for f in $(find $SEARCH_DIRS -name '*.c' 2>/dev/null | grep -v '/riscv/' | sort); do
    case "$f" in *_riscv.c) continue ;; esac
    if echo "$IN_MAKE" | grep -qxF "$f"; then continue; fi
    if [ -n "${EXCLUDED[$f]:-}" ]; then
        echo "skip: $f  (${EXCLUDED[$f]})"
        continue
    fi
    echo "FAIL: $f is not in X86_C_SRC -- it will not be compiled into the image."
    missing=$((missing+1))
done

# The reverse: a listed file that no longer exists breaks the build outright,
# which is loud rather than silent -- but catching it here names the file
# instead of leaving it to a compiler error.
stale=0
while read -r f; do
    [ -z "$f" ] && continue
    if [ ! -f "$f" ]; then
        echo "FAIL: X86_C_SRC lists $f, which does not exist."
        stale=$((stale+1))
    fi
done <<< "$IN_MAKE"

echo
if [ "$missing" -eq 0 ] && [ "$stale" -eq 0 ]; then
    echo "ok: every kernel .c is in the build, and every listed file exists."
    exit 0
fi
echo "$missing unbuilt file(s), $stale stale entr(ies)."
exit 1

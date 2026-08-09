#!/usr/bin/env bash
# tests/kernel_image_end_smoke.sh — proves the kernel-image-end guard's
# teeth bite, by stripping the reservation symbol from a copy of the image
# and asserting the guard fails on it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/kernel_image_end_check.sh asserts no allocated section lies above
# _kernel_image_end and the bootstrap stack is strictly inside it — the
# invariant that was silently false for the project's entire early life
# (the orphaned .bootstrap_stack). Its teeth were proven once, by hand, on
# a local build. Nothing ever made it fail automatically, so a regression
# that made the guard blind — a broken readelf parse, a dropped check, a
# mistyped symbol name — would pass every push unnoticed. This smoke
# proves the symbol-presence check bites.
#
# ─── Why the tooth is the missing symbol ───────────────────────────────────
# The guard's own header names the failure mode: `PROVIDE()` only defines
# _kernel_image_end when something references it, so if frame_pool.c ever
# stopped, NOTHING reserves the kernel image and the guard must say so.
# The tooth is exactly that: `objcopy --strip-symbol=_kernel_image_end`
# on a COPY of the real image, then hand the guard the copy. Every other
# property (sections, stack symbols, stack placement) is untouched, so
# exactly one check fails, with the guard's own message. The copy is
# removed afterwards; the trap removes it on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the tree on purpose and needs a build — it must
# never run in the local guard gate or a deploy gate. Run it (and the
# other build-host smokes) via tests/run_guard_smokes.sh after `make`.
#
# Exit: 0 if the tooth bit, 1 if it did not, 2 if the build is missing.
set -u

cd "$(dirname "$0")/.."   # repo root
guard=tests/kernel_image_end_check.sh
fails=0
KERNEL=my_sls_kernel.bin
TOOTH_ELF=my_sls_kernel.bin.tooth

[ -f "$KERNEL" ] || { echo "ABORT: $KERNEL not found — run 'make' first (build-host smoke)" >&2; exit 2; }
command -v objcopy >/dev/null || { echo "ABORT: objcopy not found (binutils)" >&2; exit 2; }

trap 'rm -f "$TOOTH_ELF"' EXIT

cp "$KERNEL" "$TOOTH_ELF"
objcopy --strip-symbol=_kernel_image_end "$TOOTH_ELF"

out="$(bash "$guard" "$TOOTH_ELF" 2>&1)"
rc=$?
rm -f "$TOOTH_ELF"

expected="_kernel_image_end is not in the symbol table"
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$expected"; then
    echo "TOOTH OK   missing image-end symbol — guard failed with: $expected"
    printf '%s\n' "$out" | grep -F "FAIL:" | sed 's/^/           /'
else
    echo "TOOTH FAIL missing image-end symbol — guard rc=$rc, expected failure '$expected'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Final restore check — prove the real image still passes, so this smoke
# can never leave a footprint for a later step (or a local run).
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — image clean after tooth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the copy was removed:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "kernel_image_end_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]

#!/usr/bin/env bash
# tests/no_tls_relocations_smoke.sh — proves the no-TLS-relocations
# guard's teeth bite, by synthesizing an object with a real TLS relocation
# and asserting the guard fails on it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/no_tls_relocations_check.sh asserts no object linked into the
# kernel contains a TPOFF/GOTTPOFF/DTPMOD/DTPOFF relocation — the far side
# of the __thread boundary, after every include path, macro, and flag had
# its say. Its teeth were proven once, by hand, on a local build. Nothing
# ever made it fail automatically, so a regression that made the guard
# blind — a dropped relocation type, a broken readelf parse, an empty
# sweep passing vacuously — would pass every push unnoticed. This smoke
# proves the sweep bites.
#
# ─── How the tooth is synthesized ──────────────────────────────────────────
# The guard scans a directory of .o files with readelf -r and greps for
# TLS relocation types. The tooth is a REAL object with a REAL local-exec
# TLS access — `__thread int x;` compiles to an R_X86_64_TPOFF32
# relocation (or GOTTPOFF under PIC, which the same grep catches) — built
# in a mktemp dir outside the tree, then handed to the guard as its $1.
# The build host's gcc is the same compiler family the kernel is built
# with, so the emitted relocation is exactly the class the guard exists to
# catch. The dir is removed afterwards; the trap removes it on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script dirties the tree on purpose — it must never run in the
# local guard gate or a deploy gate. Run it (and the other build-host
# smokes) via tests/run_guard_smokes.sh after `make`.
#
# Exit: 0 if the tooth bit, 1 if it did not, 2 if gcc is missing.
set -u

cd "$(dirname "$0")/.."   # repo root
guard=tests/no_tls_relocations_check.sh
fails=0

command -v gcc >/dev/null || { echo "ABORT: gcc not found" >&2; exit 2; }

TD="$(mktemp -d)"
trap 'rm -rf "$TD"' EXIT

cat > "$TD/tooth.c" <<'EOF'
__thread int tls_tooth;
int zz_get_tls(void) { return tls_tooth; }
EOF
gcc -c "$TD/tooth.c" -o "$TD/tooth.o"

out="$(bash "$guard" "$TD" 2>&1)"
rc=$?
rm -rf "$TD"

if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "TPOFF"; then
    echo "TOOTH OK   TLS relocation — guard failed with: TPOFF relocation found"
    printf '%s\n' "$out" | grep -E "FAIL:|TPOFF" | sed 's/^/           /'
else
    echo "TOOTH FAIL TLS relocation — guard rc=$rc, expected failure 'TPOFF'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# Final restore check — prove the real build objects still pass, so this
# smoke can never leave a footprint for a later step (or a local run).
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK — real objects clean after tooth, guard passes"
else
    echo "RESTORE FAIL — guard failed after the tooth dir was removed:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "no_tls_relocations_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]

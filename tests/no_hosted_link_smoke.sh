#!/usr/bin/env bash
# tests/no_hosted_link_smoke.sh — proves the no-hosted-link guard's teeth
# bite, by synthesizing genuinely hosted images and asserting the guard
# fails on them.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/no_hosted_link_check.sh asserts the kernel image is freestanding:
# zero undefined symbols (strong OR weak), no dynamic section, no ELF
# interpreter. Its teeth were proven by hand on one build. Nothing made it fail
# automatically, so a regression that made the guard blind -- a dropped
# readelf column, a wrong awk field, a dynamic check that greps sections
# but misses program headers -- would pass every push unnoticed. This
# smoke proves the three checks bite, separately and together.
#
# ─── How the teeth are synthesized ─────────────────────────────────────────
# Tooth 1 is a normal gcc dynamic build (hello world). It carries all
# three artefacts at once: a PT_INTERP, a .dynamic section, and undefined
# symbols (__libc_start_main, puts). The guard must fail on it.
#
# Tooth 2 is a -nostdlib static link with exactly one undefined symbol
# and nothing else: no interpreter, no dynamic section. It isolates the
# undefined-symbol check from the other two, so a regression in the
# readelf -s parse cannot hide behind the interpreter check.
#
# Tooth 3 is a WEAK undefined symbol -- the case a final link allows
# through. GNU ld permits a weak undefined in a final static link (even
# under --no-undefined), resolves the call site to 0 -- a NULL call in a
# kernel with no loader -- and DROPS the symbol from the output .symtab,
# so a final-image check never sees it. The diagnostic window is the
# object/partial-link stage where the symbol is still visible, and the
# guard's undefined check keys on Ndx=UND, so a WEAK UND entry trips it
# exactly like a strong one. This tooth proves the parse is
# Bind-agnostic: a regression that filtered symbols by Bind (or grepped
# for strong "U" only) would go blind to weak undefineds and pass the
# image silently.
#
# The restore check then runs the guard against the real kernel and
# requires a pass, so a broken tooth can never leave the tree looking
# failed -- and a guard gone blind cannot report "PASS" on a hosted image.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that set.
# This script synthesizes binaries in a temp dir on purpose -- it must
# never run in a deploy gate. Run it (and the other build-host smokes)
# via tests/run_guard_smokes.sh after `make`.
#
# Exit: 0 if every tooth bit, 1 otherwise, 2 if gcc is missing.
set -u
cd "$(dirname "$0")/.."   # repo root
guard=tests/no_hosted_link_check.sh
fails=0

command -v gcc >/dev/null || { echo "ABORT: gcc not found" >&2; exit 2; }

TD="$(mktemp -d)"
trap 'rm -rf "$TD"' EXIT

# ─── Tooth 1: a real dynamic binary — all three artefacts at once ──────────
cat > "$TD/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("hosted"); return 0; }
EOF
gcc "$TD/hello.c" -o "$TD/hello" 2>/dev/null

out="$(bash "$guard" "$TD/hello" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] \
   && printf '%s' "$out" | grep -q "undefined symbol" \
   && printf '%s' "$out" | grep -q "dynamic-loading"; then
    echo "TOOTH 1 OK   dynamic binary -- guard failed, flagging symbols + dynamic artefacts"
else
    echo "TOOTH 1 FAIL dynamic binary -- guard rc=$rc, expected failure on both signals"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Tooth 2: -nostdlib, one undefined symbol, no dynamic, no interp ───────
# A relocatable link (ld -r) is the only reliable way to keep an undefined
# symbol in the output: a final link refuses it, and
# --unresolved-symbols=ignore-all silently DROPS the symbol from the table
# instead of leaving it undefined. The partially-linked object carries
# exactly one artefact -- the undefined zz_extern_fn -- and no interpreter
# or dynamic section, isolating the undefined-symbol check from the other
# two so a regression in the readelf -s parse cannot hide behind them.
cat > "$TD/undef.c" <<'EOF'
int zz_extern_fn(void);
void zz_use_it(void) { zz_extern_fn(); }
EOF
gcc -c "$TD/undef.c" -o "$TD/undef.o"
ld -r "$TD/undef.o" -o "$TD/undef"

out="$(bash "$guard" "$TD/undef" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "zz_extern_fn"; then
    echo "TOOTH 2 OK   undefined symbol without dynamic artefacts -- guard named it"
else
    echo "TOOTH 2 FAIL undefined symbol -- guard rc=$rc, expected it named"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Tooth 3: a WEAK undefined symbol — the case a final link allows ──────
# See the header: weak undefineds are permitted by a final link and
# dropped from its .symtab, so the object stage is where they are still
# visible -- and where the guard must catch them, keyed on Ndx=UND
# regardless of Bind.
cat > "$TD/weak.c" <<'EOF'
extern int zz_weak_fn(void) __attribute__((weak));
int zz_call_weak(void) { return zz_weak_fn(); }
EOF
gcc -c "$TD/weak.c" -o "$TD/weak.o"

out="$(bash "$guard" "$TD/weak.o" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "zz_weak_fn"; then
    echo "TOOTH 3 OK   weak undefined symbol -- guard named it"
else
    echo "TOOTH 3 FAIL weak undefined -- guard rc=$rc, expected it named"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Restore: the real kernel must still pass ──────────────────────────────
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK   real kernel still freestanding -- guard passes"
else
    echo "RESTORE FAIL real kernel rejected:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "no_hosted_link_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]

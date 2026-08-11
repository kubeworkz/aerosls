#!/usr/bin/env bash
# tests/vendor_selfcontained_check.sh — a vendored tree must contain
# everything the build needs, and must not be able to hide any of it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# vendor/mbedtls was flattened specifically so a build host needs no Python,
# no Perl, no submodules and no code generation. Three files in it ARE
# generated -- psa_crypto_driver_wrappers.h and friends -- and they were
# generated before the prune, exactly so they would be committed with
# everything else.
#
# They were not committed. vendor/mbedtls/library/.gitignore, inherited from
# upstream, lists them: upstream generates them per build, so upstream ignores
# them. Six .gitignore files came along with the vendored tree and one of them
# silently defeated the entire point of vendoring.
#
# The failure mode is the nasty kind. My working tree had the files, so every
# check I ran locally passed. Dave's clone did not, and the build died on a
# missing header several commits later, in a file neither of us had touched.
# "It works here" was true and worthless.
#
# Two rules:
#   1. no .gitignore anywhere under vendor/ -- a vendored snapshot has no
#      build tree to keep tidy, so an ignore rule there can only ever hide
#      something that should have been committed
#   2. every .c and .h under vendor/ is TRACKED -- the direct statement of
#      what rule 1 is protecting, checked independently so that a future
#      ignore mechanism (a global gitignore, a skip-worktree bit) is caught
#      even though rule 1 would not see it
#
# Exit: 0 pass, 1 fail (never 2 -- it reads the index, always present).
set -u
cd "$(dirname "$0")/.."

[ -d vendor ] || { echo "PASS  vendor/ does not exist; nothing to check"; exit 0; }

fails=0

# ─── 1. no inherited ignore rules ──────────────────────────────────────────
ignores="$(find vendor -name .gitignore 2>/dev/null || true)"
if [ -n "$ignores" ]; then
    echo "FAIL  .gitignore found inside vendor/:"
    printf '        %s\n' $ignores
    echo "        These come from upstream, where the files they hide are"
    echo "        generated per build. Here they are committed artefacts, and"
    echo "        an ignore rule can only stop one reaching a build host."
    fails=$((fails + 1))
fi

# ─── 2. every source is actually in the index ──────────────────────────────
# Compares the filesystem against git's view directly, so it catches anything
# that keeps a file out of the index -- not just the mechanism rule 1 knows
# about.
untracked="$(git ls-files --others --exclude-standard -- vendor 2>/dev/null \
             | grep -E '\.(c|h)$' || true)"
if [ -n "$untracked" ]; then
    n="$(printf '%s\n' "$untracked" | grep -c .)"
    echo "FAIL  $n source file(s) under vendor/ are NOT tracked:"
    printf '%s\n' "$untracked" | head -10 | sed 's/^/        /'
    [ "$n" -gt 10 ] && echo "        ... and $((n - 10)) more"
    echo "        A build host that clones this repo will not have them."
    echo "        If they are generated, generate them and commit the result;"
    echo "        the vendored tree exists so nobody needs the generator."
    fails=$((fails + 1))
fi

# ─── 3. the three known-generated files, by name ───────────────────────────
# Belt and braces: rule 2 catches these only while they exist on the machine
# running the check. On a fresh clone they would simply be absent, and absent
# is not untracked. Naming them means the check fails on a clone too.
for f in library/psa_crypto_driver_wrappers.h \
         library/psa_crypto_driver_wrappers_no_static.c \
         library/ssl_debug_helpers_generated.c; do
    p="vendor/mbedtls/$f"
    [ -d vendor/mbedtls ] || continue
    if ! git ls-files --error-unmatch "$p" >/dev/null 2>&1; then
        echo "FAIL  generated file not in the index: $p"
        echo "        vendor/mbedtls/PROVENANCE.txt records the upstream commit;"
        echo "        regenerate with that revision's scripts and commit it."
        fails=$((fails + 1))
    fi
done

if [ "$fails" -eq 0 ]; then
    n="$(git ls-files -- vendor | grep -cE '\.(c|h)$' || echo 0)"
    echo "PASS  vendor/ is self-contained: no ignore rules, $n source file(s) tracked"
    exit 0
fi

echo
echo "FAIL  $fails vendor self-containment rule(s) violated"
exit 1

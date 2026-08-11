#!/usr/bin/env bash
# tests/entropy_source_check.sh — nothing but vec_index.c may touch the
# test-only PRNG, and nothing may ship a second one.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/vec_index.c contains a xorshift32 seeded with a fixed constant. That
# is correct and deliberate: HNSW layer assignment has to be reproducible for
# its host test assertions, and the function's own comment says it never
# claimed cryptographic or even statistically rigorous randomness.
#
# It is also, until entropy.c landed, the only thing in 176,000 lines that
# looked like a random number generator. The failure this guard prevents is
# not subtle and not hypothetical -- it is somebody needing "a random number"
# in a hurry, grepping for one, finding this, and using it. Every node would
# then generate byte-identical key material on every boot, TLS would handshake
# perfectly, every test would pass, and nothing would ever indicate a problem.
# That is the Debian OpenSSL failure almost exactly.
#
# The rename to vi_testonly_rand32 makes the mistake harder. This guard makes
# it fail the build. A name is a suggestion; a check is a boundary.
#
# Three rules:
#   1. vi_testonly_rand32 / vi_testonly_rng_state appear ONLY in vec_index.c
#   2. no OTHER file defines its own PRNG that looks like a general-purpose
#      random source (a small denylist of shapes, not a proof)
#   3. entropy.c's fail-closed return is not quietly discarded at call sites --
#      every entropy_get() caller must inspect the result
#
# Rule 3 matters most and is the least obvious. entropy_get() returning an
# error with the buffer untouched is only a safety property if callers CHECK
# it; a caller that ignores it gets a stack buffer's previous contents and
# proceeds, which is worse than a hard failure because it looks like it worked.
#
# Exit: 0 pass, 1 fail (never 2 -- it needs no build artefacts).
set -u
cd "$(dirname "$0")/.."

fails=0

# ─── 1. the test-only PRNG stays where it is ───────────────────────────────
# Comment lines are stripped first. entropy.h names the function in prose --
# it is the documentation telling you not to use it -- and a guard that fires
# on its own explanation is a guard people switch off.
leak=$(grep -rn "vi_testonly_rand32\|vi_testonly_rng_state" \
         --include=*.c --include=*.h kernel/ net/ arch/ drivers/ tools/ 2>/dev/null \
       | grep -v "^kernel/vec_index\.c:" \
       | grep -vE "^[^:]+:[0-9]+: *(\*|//|/\*)" \
       | cut -d: -f1 | sort -u || true)
if [ -n "$leak" ]; then
    echo "FAIL  the test-only PRNG is referenced outside kernel/vec_index.c:"
    printf '        %s\n' $leak
    echo "        It is a FIXED-SEED xorshift. If you need random bytes, use"
    echo "        kernel/entropy.h's entropy_get() and handle its error return."
    fails=$((fails + 1))
fi

# ─── 2. no second home-grown PRNG ──────────────────────────────────────────
# Shapes, not a proof: the xorshift triple-shift idiom and the classic LCG
# multipliers. Catches a copy-paste, not a determined author.
susp=$(grep -rln "x \^= x << 13\|1103515245\|6364136223846793005" \
         --include=*.c kernel/ net/ arch/ 2>/dev/null \
       | grep -v "^kernel/vec_index\.c$" \
       | grep -v "^kernel/entropy\.c$" || true)
if [ -n "$susp" ]; then
    echo "FAIL  a hand-rolled PRNG appears outside the two files allowed one:"
    printf '        %s\n' $susp
    echo "        kernel/entropy.h exists. Use it, or explain here why not."
    fails=$((fails + 1))
fi

# ─── 3. entropy_get()'s return value is never discarded ────────────────────
# A call is acceptable if the line assigns it, tests it, or returns it. A bare
# `entropy_get(buf, n);` statement is the failure: the buffer is untouched on
# error and the caller cannot tell.
bare=$(grep -rn "entropy_get *(" --include=*.c kernel/ net/ arch/ 2>/dev/null \
       | grep -v "^kernel/entropy\.c:" \
       | grep -vE "(=|==|!=|<|>|return|if *\(|while *\(|\|\||&&)" || true)
if [ -n "$bare" ]; then
    echo "FAIL  entropy_get() called without inspecting its return value:"
    printf '        %s\n' "$bare"
    echo "        On failure the buffer is UNTOUCHED. Ignoring the return means"
    echo "        using whatever was on the stack as key material."
    fails=$((fails + 1))
fi

if [ "$fails" -eq 0 ]; then
    echo "PASS  entropy sources: test-only PRNG contained, no second PRNG, no discarded returns"
    exit 0
fi
echo "FAIL  $fails entropy-source rule(s) violated"
exit 1

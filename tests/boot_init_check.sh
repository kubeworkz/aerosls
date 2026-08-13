#!/usr/bin/env bash
# tests/boot_init_check.sh — a subsystem that is linked but never started is
# not a subsystem, it is dead weight that looks like a feature.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/entropy.c was written, reviewed, mutation-tested, wired into the
# Makefile, compiled, linked, and covered by 31 passing host tests. It was
# also completely INERT: nothing in the boot path ever called entropy_init().
#
# Nothing caught it, and nothing could have. Every host test calls the init
# function itself as setup -- correct for a unit test, and exactly why none of
# them can see this: the call they need IS the call under test, and they make
# it themselves. It took booting four real nodes, a cluster-wide check, and
# four wrong diagnoses from me before the cause surfaced.
#
# That is a class of bug, not an incident. The same shape was one commit away
# from repeating with sls_tls_memory_init() -- a fixed-pool allocator that
# mbedTLS does not check before dereferencing.
#
# So this guard exists to make it a BUILD failure. It is a declared list, not
# a clever heuristic: a heuristic over "functions that look like inits" would
# have to guess which ones are subsystem entry points and which are helpers,
# and a guard that guesses is a guard people learn to override.
#
# ─── Adding to the list ────────────────────────────────────────────────────
# When you write a subsystem with an init that must run at boot, add its name
# below. If it is deliberately NOT called from kernel.c -- some are called by
# a driver, or on demand -- do not add it. The list is a statement about what
# the boot path owes, and it is only useful while it is honest.
#
# Exit: 0 pass, 1 fail, 2 abort (prerequisite missing: the call-site
# matcher runs in python3, which is the one tool this guard needs).
set -u
cd "$(dirname "$0")/.."

BOOT=kernel/kernel.c
[ -f "$BOOT" ] || { echo "FAIL: $BOOT not found."; exit 1; }

# A guard that examined nothing must not pass: without python3 the matcher
# below produces an empty report, the missing loop never fires, and the
# verdict would be PASS on a host that checked nothing. Pre-check the tool
# like every other guard pre-checks its tools (gcc, nm, readelf, curl).
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not found -- the call-site matcher runs in python." >&2
    exit 2
}

# Subsystem inits that MUST be reached from the boot path, and why they matter
# if they are not. The reason is printed on failure -- "not called" is a fact,
# "TLS will refuse every handshake" is a diagnosis.
REQUIRED="
entropy_init|no CSPRNG: TLS refuses to start and no key material can be generated
rtc_init|no wall clock: certificate validity cannot be checked
sls_tls_memory_init|mbedTLS's pool is unset; its calloc does not check before dereferencing
sls_tls_time_init|nothing reports whether TLS could start on this node
auth_init|no tokens registered: every authenticated route rejects everything
"

fails=0

# Matching is done in python, not grep, because the first version of this
# guard was fooled twice by its own tree:
#
#   1. COMMENTS. The prose in kernel.c explaining why entropy_init() is called
#      contains the string "entropy_init()". A grep for the name matched the
#      explanation and reported the call present.
#   2. DECLARATIONS. `int sls_tls_memory_init(void);` at the top of kernel.c
#      also matches "name(" -- so declaring a function counted as calling it.
#
# Both teeth passed a guard whose own header warns about exactly this. A check
# that can be satisfied by a comment about the check is worse than no check,
# because it reports PASS with authority.
#
# So: strip comments, drop declaration lines, then require the name to appear
# as a statement.
names="$(printf '%s' "$REQUIRED" | while IFS='|' read -r f w; do [ -n "$f" ] && printf '%s ' "$f"; done)"

# Names go in as ARGUMENTS, not on stdin. The first attempt piped them to a
# heredoc'd script and the printf landed after the heredoc terminator, so
# python read an empty stdin, found nothing, and reported every subsystem
# missing. Loud and obviously wrong -- but the previous bug was quiet and
# obviously RIGHT, which is the more dangerous of the two.
report="$(python3 -c '
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)     # block comments
src = re.sub(r"//[^\n]*", " ", src)                   # line comments
kept = []
for line in src.splitlines():
    # a bare prototype: <type> name(...);  -- declaring is not calling
    if re.match(r"^\s*(?:static\s+|extern\s+)?[A-Za-z_][A-Za-z0-9_ \*]*\s+[A-Za-z_][A-Za-z0-9_]*\s*\([^;{]*\)\s*;\s*$", line):
        continue
    kept.append(line)
body = "\n".join(kept)
for name in sys.argv[2:]:
    if not re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(", body):
        print(name)
' "$BOOT" $names)"

while IFS= read -r missing; do
    [ -z "$missing" ] && continue
    why="$(printf '%s' "$REQUIRED" | grep "^$missing|" | cut -d'|' -f2)"
    echo "FAIL  $missing() is never called from $BOOT"
    echo "        consequence: $why"
    fails=$((fails + 1))
done <<EOF
$report
EOF

if [ "$fails" -eq 0 ]; then
    n="$(printf '%s' "$REQUIRED" | grep -c .)"
    echo "PASS  boot init: all $n required subsystem inits are reached from $BOOT"
    exit 0
fi

echo
echo "FAIL  $fails subsystem(s) linked but never started."
echo "      This is the entropy_init() bug: the code compiles, links, and its"
echo "      host tests pass -- because host tests call init themselves. Only"
echo "      the boot path can be wrong here, and only this check looks at it."
exit 1

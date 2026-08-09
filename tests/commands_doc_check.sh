#!/usr/bin/env bash
# commands_doc_check.sh — docs/COMMANDS.md against the real command surface.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# Over one session, four commands were handed to an operator that did not
# work: `stream list` (never existed), `aeroslsctl raw /api/streams` (raw
# takes the method as a POSITIONAL argument first), `--method POST` (not a
# flag), and an upload body using `data` where the parser reads `hex`.
#
# Every one of those was a guess about a surface that is fully determined by
# source already in the tree: user/shell.c's dispatch, tools/aeroslsctl's
# argparse definitions, and net/http.c's json_str() field reads. The failure
# was not that the documentation was wrong -- 164 of 165 shell commands were
# documented correctly -- it was that the parts a person actually types were
# NOT documented, so they got reconstructed from memory each time.
#
# This script makes that class of drift a test failure. It is deliberately
# strict about the three things that were actually got wrong, rather than
# trying to validate prose.
#
# Run: bash tests/commands_doc_check.sh
set -u
cd "$(dirname "$0")/.." || exit 1

DOC=docs/COMMANDS.md
fail=0
pass=0
ok()   { echo "ok:   $1"; pass=$((pass+1)); }
bad()  { echo "FAIL: $1"; fail=$((fail+1)); }

[ -f "$DOC" ] || { echo "FAIL: $DOC missing"; exit 1; }

# ── 1. Every dispatched shell command is documented ───────────────────────
# Both matchers: sh_starts()/sh_streq()/strcmp() for commands with arguments,
# and sh_eq() for argless ones. Missing sh_eq was itself a mistake made while
# writing this check -- it hid 47 commands and made the doc look full of
# phantom entries.
missing=$(python3 - <<'PY'
import re
s = open('user/shell.c').read()
disp = sorted(set(x.strip() for x in re.findall(
    r'(?:sh_starts|sh_streq|sh_eq|!strcmp)\(input_buffer,\s*"([^"]+)"', s)))
doc = open('docs/COMMANDS.md').read()
for d in disp:
    if d not in doc:
        print(d)
PY
)
if [ -z "$missing" ]; then
    ok "every shell command in user/shell.c's dispatch appears in $DOC"
else
    bad "shell commands dispatched but NOT documented:"
    echo "$missing" | sed 's/^/        /'
fi

# ── 2. Every aeroslsctl subcommand is documented ──────────────────────────
# This is the half that was missing entirely, and the direct cause of three
# of the four bad commands.
missing_ctl=$(python3 - <<'PY'
import re
s = open('tools/aeroslsctl').read()
subs = sorted(set(re.findall(r'sub\.add_parser\("([a-z-]+)"', s)))
doc = open('docs/COMMANDS.md').read()
for c in subs:
    if f"aeroslsctl {c}" not in doc and f"`{c}`" not in doc:
        print(c)
PY
)
if [ -z "$missing_ctl" ]; then
    ok "every aeroslsctl subcommand is documented"
else
    bad "aeroslsctl subcommands not documented:"
    echo "$missing_ctl" | sed 's/^/        /'
fi

# ── 3. `raw` is documented with the METHOD FIRST, and by EXAMPLE ──────────
# The specific mistake, encoded -- with a caveat worth recording. The correct
# form `raw GET|POST <path> [--body JSON]` was ALREADY in this document when
# all four bad commands were typed. Having the grammar written down did not
# prevent the error, because a bullet-list grammar is skimmed. So this check
# requires a copy-pasteable example with a literal method, not just the
# grammar: the thing that actually gets read is the thing you can paste.
if grep -qE 'aeroslsctl.*raw +(GET|POST) +/' "$DOC"; then
    ok "raw is documented with a literal, pasteable method before the path"
else
    bad "no concrete 'aeroslsctl raw GET /path' example in $DOC -- the grammar"
    bad "  alone has already failed to prevent this mistake once"
fi
if grep -qE 'raw +GET\|POST' "$DOC"; then
    ok "...and the grammar form is present too"
else
    bad "the 'raw GET|POST <path>' grammar line is missing from $DOC"
fi
# A counter-example is GOOD documentation -- "here is the thing that fails"
# is often what a reader needs most. So the test is not "never mentions
# --method", it is "never mentions it as though it worked": every occurrence
# must sit on a line marked as wrong, or inside a block whose preceding line
# marks it so. The first version of this check simply banned the string and
# failed against the very section written to prevent the mistake.
bad_method=$(python3 - <<'PY'
import re
lines = open('docs/COMMANDS.md').read().split('\n')
NEG = re.compile(r'wrong|fails?|no such|not an option|reject|error|avoid|instead of', re.I)
for i, ln in enumerate(lines):
    if '--method' not in ln:
        continue
    window = lines[max(0, i-4):i+2]
    if not any(NEG.search(w) for w in window):
        print(f"{i+1}: {ln.strip()}")
PY
)
if [ -z "$bad_method" ]; then
    ok "--method appears only as a counter-example, never as usable syntax"
else
    bad "--method presented as though raw accepts it (it does not):"
    echo "$bad_method" | sed 's/^/        /'
fi

# ── 4. Upload body field is named `hex`, not `data` ───────────────────────
# api_stream_upload() reads json_str(body,"hex",...). Documenting `data`
# produces "name and hex required" at runtime.
if grep -q '"hex"' "$DOC"; then
    ok "the stream upload body is documented with its real 'hex' field"
else
    bad "$DOC does not document the 'hex' field that /api/stream/upload requires"
fi

# ── 5. No phantom stream shell command ────────────────────────────────────
# There is no `stream` command in the dispatch at all. Streams are reached
# over REST. If a future phase adds one, this check should be updated -- but
# it must not be documented before it exists.
if python3 -c "
import re,sys
s=open('user/shell.c').read()
d=set(re.findall(r'(?:sh_starts|sh_streq|sh_eq|!strcmp)\(input_buffer,\s*\"([^\"]+)\"', s))
sys.exit(0 if any(x.strip().startswith('stream') for x in d) else 1)"; then
    ok "a 'stream' shell command now exists -- update this check and the doc"
else
    phantom=$(python3 - <<'PY'
import re
lines = open('docs/COMMANDS.md').read().split('\n')
NEG = re.compile(r'no |not |never|refus|does not exist|wrong|fails?|only over REST', re.I)
for i, ln in enumerate(lines):
    if not re.search(r'stream (list|ls)\b', ln):
        continue
    window = lines[max(0, i-4):i+2]
    if not any(NEG.search(w) for w in window):
        print(f"{i+1}: {ln.strip()}")
PY
)
    if [ -z "$phantom" ]; then
        ok "'stream list' appears only as a named non-existent command"
    else
        bad "a 'stream list'-style shell command is documented as though it works:"
        echo "$phantom" | sed 's/^/        /'
    fi
fi

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1

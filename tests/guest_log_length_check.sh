#!/usr/bin/env bash
# tests/guest_log_length_check.sh — every host.log call in the polyglot guest
# must log exactly its text, no more and no less.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# user/polyglot/guest/calc_guest.wat talks to the host through an explicitly
# length-delimited import — host.log(ptr, len) — with the payload in a `data`
# segment whose trailing `\00` is a vestigial C-ism: nothing reads those
# strings as C strings, the import takes the length. So the two numbers that
# decide what lands in the log are both hand-written in the WAT, and both
# were wrong at once:
#
#   * `(call $log (i32.const 64) (i32.const 34))` logged 34 bytes for the
#     29-byte `PASS add(2,3)=5 sqrt verified`: the 30th byte is the segment's
#     NUL and bytes 31-34 are the zero-fill to the next segment (the segment
#     sits at 64, the next at 160). Five NUL bytes therefore reached the test
#     harness's stdout, which is why every CI run printed
#
#       warning: command substitution: ignored null byte in input
#
#     next to the e2e step — alarming, and read for a while as a symptom of
#     the intermittent failure it has nothing to do with. It is also how a
#     log line can lose bytes without anyone noticing.
#
#   * `(call $log (i32.const 288) (i32.const 27))` logged 27 bytes for the
#     28-byte `PASS async heavy_reduce=2016`, so the CI log showed
#     `[guest] PASS async heavy_reduce=201` — a PASS line asserting a sum of
#     201 where the guest had verified 2016. Quietly truncated, in the one
#     place a reader looks to confirm the demo did the right thing.
#
# Neither is visible without comparing the two numbers, and `wasm-sidecar`
# embeds the WAT with include_str!, so a wrong length ships in a binary that
# boots and passes. The fix is not "be careful with lengths": it is a check
# whose whole job is comparing them.
#
# GUARD-KIND: host (plain bash + python3; reads one file in the tree).
#
# Exit: 0 pass, 1 fail (a log call's length does not match its text), 2 abort
# (missing prerequisite).
#
# The negative control at the end is not decoration: it re-runs the same
# audit over a copy of the WAT with one length shifted by a byte and requires
# the audit to report it, so this guard cannot pass by having gone blind.
set -u
cd "$(dirname "$0")/.."   # repo root

WAT="user/polyglot/guest/calc_guest.wat"

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$WAT" ] || { echo "ABORT: $WAT not found" >&2; exit 2; }

python3 - "$WAT" <<'PY'
import re
import sys
import tempfile

WAT = sys.argv[1]

SEG_RE = re.compile(r'\(data \(i32\.const (\d+)\) "([^"]*)"\)')
LOG_RE = re.compile(r'\(call \$log \(i32\.const (\d+)\) \(i32\.const (\d+)\)\)')


def decode(s):
    """Decode a WAT string literal's \\XX escapes into bytes."""
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 2 < len(s):
            out.append(int(s[i + 1:i + 3], 16))
            i += 3
        else:
            out.append(ord(s[i]))
            i += 1
    return bytes(out)


def audit(path):
    """(checked, violations) for every host.log call in the WAT.

    A violation is a call whose len is not the byte length of the text at
    ptr (the segment up to its first NUL). Overshooting prints the NUL and
    the zero-fill past the segment; undershooting truncates the line.
    """
    src = open(path).read()
    seg = {int(m.group(1)): decode(m.group(2)) for m in SEG_RE.finditer(src)}
    checked, bad = 0, []
    for m in LOG_RE.finditer(src):
        ptr, ln = int(m.group(1)), int(m.group(2))
        line = src[:m.start()].count('\n') + 1
        checked += 1
        b = seg.get(ptr)
        if b is None:
            bad.append((line, ptr, ln, None,
                        "no data segment at this ptr — the call logs zero-filled memory"))
            continue
        text = b.split(b'\x00')[0]
        if ln != len(text):
            kind = ("overshoot: prints %d NUL/zero byte(s) — a capturing shell warns "
                    "'ignored null byte in input'" % (ln - len(text))
                    if ln > len(text)
                    else "undershoot: truncates the last %d byte(s) of the line"
                         % (len(text) - ln))
            bad.append((line, ptr, ln, len(text), kind))
    return checked, bad


checked, bad = audit(WAT)
print("=== %s: host.log lengths vs their text ===" % WAT)
print()
if checked == 0:
    print("FAIL: no host.log calls found — the parser or the guest changed shape")
    sys.exit(1)

for line, ptr, ln, text_len, kind in bad:
    if text_len is None:
        print("FAIL  %s:%d  log(ptr=%d, len=%d) — %s" % (WAT, line, ptr, ln, kind))
    else:
        print("FAIL  %s:%d  log(ptr=%d, len=%d) but the text is %d bytes — %s"
              % (WAT, line, ptr, ln, text_len, kind))
if bad:
    print()
    print("%d of %d host.log call(s) do not log their text." % (len(bad), checked))
    sys.exit(1)

print("ok: all %d host.log call(s) log exactly their text (%d bytes or fewer each)."
      % (checked, max(len(v.split(b'\x00')[0]) for v in
                      [decode(m.group(2)) for m in SEG_RE.finditer(open(WAT).read())])))
print()

# ── negative control: the audit must catch a one-byte shift ────────────────
# Shift the first log length by -1 and by +1 in a scratch copy; each must be
# reported. Without this, a parser that quietly matched nothing would read as
# a clean pass.
src = open(WAT).read()
base = LOG_RE.search(src)
assert base is not None, "no log call to mutate — the parser changed shape"
ok = True
for delta, label in ((-1, "undershoot"), (+1, "overshoot")):
    new_len = max(0, int(base.group(2)) + delta)
    mutated = src[:base.start(2)] + str(new_len) + src[base.end(2):]
    with tempfile.NamedTemporaryFile("w", suffix=".wat", delete=False) as fh:
        fh.write(mutated)
        path = fh.name
    _, mut_bad = audit(path)
    if not mut_bad:
        print("FAIL: the audit did not report a %s of one byte — it has gone blind" % label)
        ok = False
    else:
        print("ok: negative control — a %s of one byte is reported (%s)"
              % (label, mut_bad[0][4].split(':')[0]))
sys.exit(0 if ok else 1)
PY
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

echo "guest_log_length_check: done, 0 violations"
exit 0

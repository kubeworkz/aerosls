#!/usr/bin/env python3
"""tests/phase5_boot_markers.py — decides whether a Phase 5 boot log reached
its end state.

The flake guard (tests/phase5_boot_flake_guard.sh) and its teeth
(tests/phase5_boot_flake_guard_smoke.sh) both need the same answer to one
question: did this boot finish? One implementation is shared on purpose —
the guard's verdict and the smoke's fixtures cannot drift apart when there is
only one classifier to drift.

─── The three markers ────────────────────────────────────────────────────

  1. 'ALL PHASES PASS' — the whole irqtest sequence ran (bind, timer edges,
     serial IRQ4 loopback, stuck-driver window, watchdog respawn + clean
     re-bind, unbind before exit).

  2. 'System ready' — init.rc ran to its end: nettest, irqtest, devtest and
     caps all passed their gates (any FAIL aborts the script before this
     line). A *functional* marker, emitted by the caps applet.

  3. a '$' of the shell prompt after marker 2 — the interactive sh is up.

A boot finished when markers 1 and 3 are both present. A boot that carries
only 1 stopped in the phase chain; the classifier reports every marker it
saw, so a failure names the step instead of just "the flake is back".

Marker 2 is deliberately NOT part of the verdict, only part of the report.
It is the most fragile text in the log: its line is written by the sidecar's
console drain while the kernel's own writers share the UART, and the
measured damage across 185 logs ranges from one byte ('ystem ready') to a
splintered line ('$ $ stem ready'). The phase marker plus the prompt sit on
either side of it — 'ALL PHASES PASS' comes from irqtest, the prompt only
exists once init.rc has finished — so requiring 1 and 3 still requires the
boot to have run the whole chain, without betting the verdict on the one
line the console demonstrably mangles.

─── Why the markers are matched with one-byte tolerance ───────────────────

The serial log is a byte stream produced by more than one writer, and it is
measurably not byte-faithful. Across the 162 surviving boot logs:

  * 'ALL PHASES PASS'  — intact in 162/162.
  * 'System ready'     — intact in 131, and in 27 more it arrived ONE byte
                         shorter ('ystem ready': the leading 'S' gone, the
                         rest of the line whole), in 4 a byte was spliced in
                         ('Sys\ntem ready'), and in others the line was
                         splintered outright ('$ $ stem ready').
  * the prompt         — 83 logs end '\n\n$ ', 12 '\n$ $', 10 '\n$$ ':
                         the prompt's own bytes, in three orders.

None of that is the boot's fault: in every one of those logs the boot had
reached 'System ready', QEMU was alive, and the log simply stopped being
byte-exact. The old guard required a literal 'newline + "$ "' and so failed
the ten '$$ ' boots — its own false-negative class.

So each marker is matched intact **or** with a single damaged byte
(substituted or dropped), and the result line reports which style matched
(damage=1, torn=1) so the console's fidelity problem stays visible instead of
being silently absorbed. One damaged byte cannot turn a boot that stopped
mid-chain into a boot that finished: a missing phase marker or a missing
ready marker stays missing.

The prompt test keeps the newline anchor that stops 'echo $?' from reading
as a prompt, and adds the two shapes the shell's own interleave produces.

─── Exit ─────────────────────────────────────────────────────────────────
  --check LOG   print the marker line, exit 0 = boot finished, 1 = not.
                A log that does not exist yet is "not finished" (exit 1) —
                the guard polls, so a boot in progress must not read as done.
  --print LOG   print the marker line, exit 0 (verdict not applied).
  usage error   exit 2.
"""
import os
import re
import sys

PHASES = b"ALL PHASES PASS"
READY = b"System ready"


def _damaged_patterns(marker):
    """Regexes for the marker with exactly one byte damaged.

    All three kinds the logs actually show:
      substitution  `S.stem ready`   (one byte replaced)
      deletion      `ystem ready`    (one byte dropped — the 'System ready'
                                      case, 27 of 158 logs)
      insertion     `Sys\ntem ready` (one byte spliced in from the other
                                      writer — the 'Sys\ntem ready' case in
                                      four interleaved nolock-arm logs)
    The tolerance is one byte, it is always reported as damage, and it can
    never manufacture a marker that is absent.
    """
    out = []
    for i in range(len(marker)):
        out.append(re.escape(marker[:i]) + b"." + re.escape(marker[i + 1:]))
        out.append(re.escape(marker[:i]) + re.escape(marker[i + 1:]))
        out.append(re.escape(marker[:i]) + b"." + re.escape(marker[i:]))
    return out


def _find(data, marker):
    """(present, damaged, end_index) for a marker, intact or 1-byte damaged."""
    i = data.find(marker)
    if i >= 0:
        return 1, 0, i + len(marker)
    for pat in _damaged_patterns(marker):
        # re.S: an inserted byte is often a '\n' (the other writer's line
        # terminator), so '.' must be able to match it.
        m = re.search(pat, data, re.S)
        if m:
            return 1, 1, m.end()
    return 0, 0, -1


def classify(path):
    """Return a dict of markers for a boot log, or None if it cannot be read."""
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return None

    phases, phases_dmg, _ = _find(d, PHASES)
    ready, ready_dmg, _ = _find(d, READY)

    # The prompt: the shell's '$ ' with the newline anchor that keeps applet
    # text ('echo $?') from counting, plus the two shapes its own two writes
    # produce — '$$ ' (glued, reported as torn) and a '$' cut off at the end
    # of the log by the guard killing QEMU mid-write.
    prompt = 0
    torn = 0
    for m in re.finditer(rb"\$ ?", d):
        i = m.start()
        if i == 0 or d[i - 1:i] == b"\n":
            prompt = 1
        elif d[i - 1:i] == b"$":
            prompt = 1
            torn = 1

    return {
        "phases": phases,
        "phases_damaged": phases_dmg,
        "ready": ready,
        "ready_damaged": ready_dmg,
        "prompt": prompt,
        "torn": torn,
        "tail": d[-48:],
    }


def finished(m):
    """A boot finished when the phase chain ran and the shell prompt is up."""
    return bool(m["phases"] and m["prompt"])


def format_markers(m):
    if m is None:
        return "markers=unreadable"
    return ("phases=%d%s ready=%d%s prompt=%d torn=%d tail=%r"
            % (m["phases"], "*" if m["phases_damaged"] else "",
               m["ready"], "*" if m["ready_damaged"] else "",
               m["prompt"], m["torn"], m["tail"]))


def main(argv):
    if len(argv) != 3 or argv[1] not in ("--check", "--print"):
        sys.stderr.write("usage: phase5_boot_markers.py --check|--print LOG\n")
        return 2
    mode, path = argv[1], argv[2]
    if not os.path.exists(path):
        if mode == "--print":
            print("markers=absent (no log written yet)")
            return 0
        return 1
    m = classify(path)
    if m is None:
        return 2
    print(format_markers(m))
    if mode == "--print":
        return 0
    return 0 if finished(m) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

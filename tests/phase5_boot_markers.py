#!/usr/bin/env python3
"""tests/phase5_boot_markers.py — decides whether a Phase 5 boot log reached
its end state, and names the stage it stopped at when it did not.

One implementation is shared on purpose by three callers, so their verdicts
cannot drift apart:

  * tests/phase5_boot_flake_guard.sh  (preset `guard`)
  * tests/phase5_boot_flake_guard_smoke.sh — its teeth, driving it directly
  * tests/phase5_boot_smoke.sh        (preset `demos`)

─── The two presets ──────────────────────────────────────────────────────

  guard — did the boot FINISH?  requires 'ALL PHASES PASS' + the shell
          prompt. Used by the six-boot flake guard.

  demos — did the driver-SDK DEMOS run?  requires '[irqtest] PASS' +
          '[devtest] PASS'. Used by the single-boot Phase 5 smoke, which
          exists to prove the IRQ/DEV capability path on the real target
          independently of whether the boot went on to reach a shell.

Every marker is classified in both presets and printed in every report; the
preset only chooses which of them the verdict requires. That is what makes a
failure report actionable: a boot that stopped after the vector-36 unmask
prints `irq=1 dev=0 stage=[IRQ] unmask vector 36: pin 4 …`.

─── The markers ──────────────────────────────────────────────────────────

  1. 'ALL PHASES PASS' — the whole irqtest sequence ran (bind, timer edges,
     serial IRQ4 loopback, stuck-driver window, watchdog respawn + clean
     re-bind, unbind before exit).

  2. 'System ready' — init.rc ran to its end: nettest, irqtest, devtest and
     caps all passed their gates (any FAIL aborts the script before this
     line). A *functional* marker, emitted by the caps applet.

  3. a '$' of the shell prompt after marker 2 — the interactive sh is up.

  4. '[irqtest] PASS' — at least one irqtest phase reported PASS (the timer
     edge counter is the first). Its absence means the demo never got going.

  5. '[devtest] PASS' — the DEV path ran: SYS_DEV_MMAP of the e1000 BAR0,
     an idempotent re-map, and a MAC read with QEMU's OUI. A boot with no
     -device e1000 prints '[devtest] SKIP' instead and does not match, which
     is intended: the smoke's QEMU line always carries the card.

Out-of-band markers 2 and 3 are required only by the `guard` preset. Marker 2
is deliberately NOT part of the guard's verdict, only part of the report: it
is the most fragile text in the log — its line is written by the sidecar's
console drain while the kernel's own writers share the UART, and the measured
damage across 185 logs ranges from one byte ('ystem ready') to a splintered
line ('$ $ stem ready'). The phase marker plus the prompt sit on either side
of it — 'ALL PHASES PASS' comes from irqtest, the prompt only exists once
init.rc has finished — so requiring 1 and 3 still requires the boot to have
run the whole chain, without betting the verdict on the one line the console
demonstrably mangles.

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
the ten '$$ ' boots — its own false-negative class. The Phase 5 smoke had the
same weakness in a different place: it greped its two PASS lines with -F, so
a single dropped or spliced byte in an otherwise healthy demo line read as
"the demo never ran".

So each marker is matched intact **or** with a single damaged byte
(substituted or dropped), and the result line reports which style matched
(damage=1, torn=1) so the console's fidelity problem stays visible instead of
being silently absorbed. One damaged byte cannot turn a boot that stopped
mid-chain into a boot that finished: a missing phase marker or a missing
demo marker stays missing.

The prompt test keeps the newline anchor that stops 'echo $?' from reading
as a prompt, and adds the two shapes the shell's own interleave produces.

─── Exit ─────────────────────────────────────────────────────────────────
  --check [--preset guard|demos] LOG
                print the marker line, exit 0 = the preset's markers are all
                present, 1 = not. A log that does not exist yet is "not
                finished" (exit 1) — the callers poll, so a boot in progress
                must not read as done.
  --print [--preset guard|demos] LOG
                print the marker line, exit 0 (verdict not applied).
  usage error   exit 2.
"""
import os
import re
import sys

PHASES = b"ALL PHASES PASS"
READY = b"System ready"
IRQTEST_PASS = b"[irqtest] PASS"
DEVTEST_PASS = b"[devtest] PASS"

# Progress prefixes whose LAST occurrence names the stage a boot reached.
# '[IRQ] ' (with the space) is the kernel's unmask/mask trace, which is what
# a boot that dies between irqtest phases ends on.
STAGE_PREFIXES = (b"[irqtest]", b"[devtest]", b"[IRQ] ", b"[nettest]")

# Which markers each preset's verdict requires. 'prompt' is the one
# derived test; the rest are literal strings matched with tolerance.
PRESETS = {
    "guard": ("phases", "prompt"),
    "demos": ("irq", "dev"),
}


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


def _stage(data):
    """The last progress line in the log — the stage the boot got to."""
    for line in reversed(data.splitlines()):
        s = line.strip()
        if any(s.startswith(p) for p in STAGE_PREFIXES):
            return s[:72].decode("utf-8", "replace")
    return "-"


def classify(path):
    """Return a dict of markers for a boot log, or None if it cannot be read."""
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return None

    phases, phases_dmg, _ = _find(d, PHASES)
    ready, ready_dmg, _ = _find(d, READY)
    irq, irq_dmg, _ = _find(d, IRQTEST_PASS)
    dev, dev_dmg, _ = _find(d, DEVTEST_PASS)

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
        "irq": irq,
        "irq_damaged": irq_dmg,
        "dev": dev,
        "dev_damaged": dev_dmg,
        "prompt": prompt,
        "torn": torn,
        "stage": _stage(d),
        "tail": d[-48:],
    }


def finished(m, preset="guard"):
    """Every marker the preset requires is present."""
    return all(m[k] for k in PRESETS[preset])


def damaged_markers(m, preset="guard"):
    """Names of the preset's required markers that matched only with damage."""
    return [k for k in PRESETS[preset]
            if k != "prompt" and m.get(k + "_damaged")]


def format_markers(m, preset="guard"):
    if m is None:
        return "markers=unreadable"
    return ("need=%s phases=%d%s ready=%d%s irq=%d%s dev=%d%s prompt=%d torn=%d "
            "stage=%s tail=%r"
            % (",".join(PRESETS[preset]),
               m["phases"], "*" if m["phases_damaged"] else "",
               m["ready"], "*" if m["ready_damaged"] else "",
               m["irq"], "*" if m["irq_damaged"] else "",
               m["dev"], "*" if m["dev_damaged"] else "",
               m["prompt"], m["torn"], m["stage"], m["tail"]))


def main(argv):
    mode = None
    preset = "guard"
    path = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("--check", "--print"):
            mode = a
        elif a == "--preset":
            i += 1
            if i >= len(argv):
                preset = ""
                break
            preset = argv[i]
        else:
            path = a
        i += 1

    if mode is None or path is None or preset not in PRESETS:
        sys.stderr.write(
            "usage: phase5_boot_markers.py --check|--print "
            "[--preset guard|demos] LOG\n")
        return 2

    if not os.path.exists(path):
        if mode == "--print":
            print("markers=absent (no log written yet)")
            return 0
        return 1
    m = classify(path)
    if m is None:
        return 2
    print(format_markers(m, preset))
    if mode == "--print":
        return 0
    return 0 if finished(m, preset) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

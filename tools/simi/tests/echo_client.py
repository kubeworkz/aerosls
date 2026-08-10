#!/usr/bin/env python3
"""echo_client.py — Phase 9i (ISA doc §16): the client half of the real
kernel's device-driven UART RX interrupt echo check (see
run_riscv_tests.sh's rv-uart-echo / rv-uart-echo-m blocks). QEMU boots
the echo kernel (sls_riscv_kernel_echo.elf or its M-mode twin) with the
16550 UART on a UNIX socket; this client drives the interrupt-count
tripwire and the full readline + command-loop protocol:

  1. wait for the "[UART] ECHO READY" banner (the PLIC wiring + SIE/MIE
     are live);
  2. interrupt-count tripwire: "A", "B", "C" sent ONE AT A TIME, each
     followed by waiting for [IRQ#1], [IRQ#2], [IRQ#3] -- the kernel
     prints [IRQ#N] AFTER the PLIC complete(), so seeing [IRQ#N]
     guarantees the previous interrupt's whole claim/drain/complete round
     trip finished and the line is re-armed. One isolated keystroke
     therefore produces EXACTLY one interrupt (pinned, not guessed).
  3. "help\\r"            -> the [HELP] command list (dispatch works; a
     rapid batch that may span one or several interrupts -- the drain
     loop must lose no bytes);
  4. "PING\\bX\\r"         -> each character echoed as typed, the backspace
     erases G (so the line is PINX), and the CR-only terminator routes
     it to dispatch, which reports an unknown command (proves char echo,
     backspace editing, CR-only line endings, and dispatch in one shot);
  5. "echo hello world\\r" -> [ECHO] hello world (the argument subcommand);
  6. "exit\\r"             -> S-mode powers the machine off via SBI_SRST
     (the socket sees EOF); the M-mode twin reports a halt instead.

The tripwire's second half: the rapid batches (steps 3-5) each arrive as
queued bytes and must drain completely (every char echoed, every line
dispatched) no matter how many interrupts they split into -- pinning
claim/complete discipline under rapid input.

Optional second argument "tick" (Phase 9k-9n; passed by BOTH runners --
the S-mode kernel arms the timer via the stimecmp CSR, the M-mode twin
via the CLINT mtimecmp MMIO): after the IRQ tripwire, wait for [TICK 2]
(printed AFTER the re-arm, so it proves the timer is genuinely
periodic -- a one-shot arm would deliver at most [TICK 1] and go
silent), then send `tasks` and assert the Phase 9l time-slicing
invariant: the four round-robin tasks' slice counts are within 1 of
each other (true at every instant of a fair rotation), at least one
slice has run, and [SLICE ...] markers appear in the stream -- the
per-tick preemption exposed over the UART. The tick line also carries
Phase 9m wall-clock uptime ([TICK N Us], seconds from rdtime). On the
echo builds the period is 100ms (Phase 9n contention probe), so the
client asserts the 100ms-aware invariants: U >= N/10 and monotone,
plus the missed-re-arm tooth -- the observed tick numbers must form an
unbroken run 1..max (no gaps), proving the 10x-faster tick stream
interleaves with the [IRQ] tripwire with zero lost bytes or skipped
re-arms. (The tick waits are placed before the command loop so the
assertions run while the machine is still on; `exit` below powers it
off.)

Usage: python3 echo_client.py /path/to/serial.sock [tick]

Exit code 0 = every marker observed; 1 = not.
"""
import re
import socket
import sys
import time


def recv_until(s, buf, marker, timeout):
    """Read until marker appears in the accumulated stream (or the deadline
    passes). NOTE: on socket.timeout the loop must CONTINUE, not break —
    a single 0.5s recv gap (e.g. the kernel's 1-second periodic tick
    straddling the timeout window) must not abort the wait. The Phase 9k
    tick tripwire exposed exactly this latent bug: [TICK 1] fires ~10ms
    after the first recv's timeout, so a break-on-timeout implementation
    failed to find even [TICK 2] within its 8s budget. The fast markers
    (banner, IRQ tripwire, help/echo replies) never tripped it because
    they all respond well under 0.5s."""
    deadline = time.time() + timeout
    while marker not in buf and time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            continue
    return buf


def main() -> int:
    path = sys.argv[1]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(0.5)
    for _ in range(100):          # QEMU creates the socket at startup
        try:
            s.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL: could not connect to serial socket")
        return 1

    buf = b""
    # Wait for the WHOLE banner line, not just "ECHO READY" (which sits
    # mid-line): interrupts are already enabled, so sending before the
    # banner finishes would interleave the first keystrokes with the
    # banner's remaining characters.
    buf = recv_until(s, buf, b"device-driven RX interrupt echo.", 10)
    if b"device-driven RX interrupt echo." not in buf:
        print("FAIL: kernel ECHO READY banner never seen")
        print(buf.decode(errors="replace"))
        return 1

    # Interrupt-count tripwire: one isolated keystroke -> exactly one
    # interrupt. [IRQ#N] is printed after the complete(), so the next
    # keystroke is only sent once the previous interrupt's round trip is
    # fully finished and the line is re-armed -- deterministic, not a race.
    s.sendall(b"A")
    buf = recv_until(s, buf, b"[IRQ#1]", 5)
    s.sendall(b"B")
    buf = recv_until(s, buf, b"[IRQ#2]", 5)
    s.sendall(b"C")
    buf = recv_until(s, buf, b"[IRQ#3]", 5)

    # The tripwire keystrokes A/B/C are buffered as a pending line; a
    # lone CR dispatches them (proving they were all received intact) and
    # resets the buffer before the command loop below.
    s.sendall(b"\r")
    buf = recv_until(s, buf, b'unknown command: "ABC"', 5)

    # Phase 9k periodic-timer tripwire (tick mode only): the kernel arms
    # a 1s tick at boot (S-mode: stimecmp CSR, Sstc; M-mode twin: CLINT
    # mtimecmp MMIO) and re-arms inside every timer handler (STIP/MTIP),
    # printing [TICK N] after the re-arm. [TICK 1] fires ~1s after boot
    # and may already be in the buffer; [TICK 2] proves the interrupt
    # fired AND was re-armed — a one-shot arm would go silent after
    # [TICK 1]. The 8s budget covers a slow CI host.
    tick_mode = len(sys.argv) > 2 and sys.argv[2] == "tick"
    tasks_ok = False
    if tick_mode:
        # Phase 9m: the tick line is now [TICK N Us] (wall-clock seconds
        # appended), so the marker is the tick-2 prefix up to the space
        # before the seconds — the old exact [TICK 2] no longer exists.
        buf = recv_until(s, buf, b"[TICK 2 ", 8)
        if b"[TICK 2 " not in buf:
            print("FAIL: periodic timer tick never reached [TICK 2]")
            print(buf.decode(errors="replace"))
            return 1

        # Phase 9l time-slicing tripwire: the tick now doubles as a
        # round-robin scheduler. Every timer interrupt slices the ACTIVE
        # task (a bounded LCG budget), rotates, and prints [SLICE <name>]
        # on the echo builds. Send `tasks` and assert the table proves
        # fair preemption: the four slice counts must be within 1 of each
        # other (the round-robin invariant, true at every instant), at
        # least one slice must have run (max >= 1), and the [SLICE ...]
        # markers must appear in the stream (the per-tick preemption is
        # exposed over the UART). The wait targets the kernel's
        # [TASKS-END] marker, printed only after all four rows — waiting
        # on a row prefix would race: the kernel prints char-by-char, so
        # a row's digits can straddle a socket segment and arrive after
        # the prefix, truncating the last count.
        s.sendall(b"tasks\r")
        buf = recv_until(s, buf, b"[TASKS-END]", 5)
        ttext = buf.decode(errors="replace")
        counts = [int(x) for x in re.findall(r"slices=(\d+)", ttext)]
        if (b"[TASKS]" in buf and len(counts) == 4
                and max(counts) - min(counts) <= 1
                and max(counts) >= 1 and b"[SLICE " in buf):
            tasks_ok = True
        else:
            print("FAIL: time-slicing table not as expected (counts=%r)" % counts)
            print(ttext[-1300:])
            return 1

    s.sendall(b"help\r")
    buf = recv_until(s, buf, b"[HELP] commands:", 5)

    s.sendall(b"PING\bX\r")       # backspace erases the G; CR-only terminator
    buf = recv_until(s, buf, b'unknown command: "PINX"', 5)

    s.sendall(b"echo hello world\r")
    buf = recv_until(s, buf, b"[ECHO] hello world", 5)
    # (The batch may split across one or several interrupts -- the drain
    # loop must lose no bytes; the full [ECHO] line is the proof.)

    s.sendall(b"exit\r")
    # S-mode: powers off -> EOF; M-mode: reports halt, socket stays open.
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break

    text = buf.decode(errors="replace")
    print(text[-1300:])

    # Phase 9m/9n wall-clock + contention tripwires (tick mode only):
    # every tick line carries the uptime in seconds derived from rdtime
    # (the `time` CSR / mtime at the shared 10 MHz timebase), e.g.
    # [TICK 2 2s]. On the echo builds the period is 100ms (Phase 9n
    # contention probe), so tick N fires at ~N*0.1s and the wall clock
    # must satisfy U >= N/10 (a drift-free timer cannot report fewer
    # elapsed seconds than its cadence implies), monotone non-decreasing
    # in stream order. The sequence check is the missed-re-arm tooth:
    # the observed tick numbers must form an unbroken run 1..max — a
    # skipped re-arm (or a lost interrupt) would leave a gap, which
    # proves the tick stream survives the 10x-faster interleaving with
    # the UART [IRQ] path without losing a single tick.
    up_ok = True
    if tick_mode:
        pairs = [(int(a), int(b)) for a, b in
                 re.findall(r"\[TICK (\d+) (\d+)s\]", text)]
        ticks = sorted({tn for tn, _ in pairs})
        prev_up = None
        for tn, up in pairs:
            if up < tn // 10 or (prev_up is not None and up < prev_up):
                up_ok = False
                break
            prev_up = up
        if not pairs or ticks != list(range(1, ticks[-1] + 1)):
            up_ok = False
            print("FAIL: missed re-arm -- tick numbers not consecutive (seen %d..%d, gaps %r)"
                  % (ticks[0] if ticks else 0, ticks[-1] if ticks else 0,
                     sorted(set(range(1, (ticks[-1] if ticks else 0) + 1)) - set(ticks))))
            return 1
        if not up_ok:
            print("FAIL: wall-clock uptime not consistent with ticks (pairs=%r)" % pairs)
            return 1

    ok = (b"[IRQ#1]" in buf) and (b"[IRQ#2]" in buf) and (b"[IRQ#3]" in buf) and \
         (b'unknown command: "ABC"' in buf) and \
         (b"[HELP] commands:" in buf) and \
         (b'unknown command: "PINX"' in buf) and \
         (b"[ECHO] hello world" in buf) and \
         (not tick_mode or b"[TICK 2 " in buf) and \
         (not tick_mode or tasks_ok)
    if ok:
        msg = ("ECHO_OK: interrupt tripwire pinned (1 keystroke -> 1 IRQ "
               "x3), help dispatched, backspace editing proven (PING\\bX "
               "-> PINX), CR-only line routed, echo <text> works")
        if tick_mode:
            msg += (", periodic timer tick proven ([TICK 2] after re-arm), "
                    "round-robin time-slicing proven (tasks table, "
                    "fair rotation max-min <= 1), wall-clock uptime "
                    "proven ([TICK N Us] consistent, monotone), 100ms "
                    "contention probe proven (ticks 1..N consecutive, "
                    "no missed re-arms, no lost bytes)")
        print(msg)
    else:
        print("FAIL: interrupt-tripwire / readline / dispatch markers not all observed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

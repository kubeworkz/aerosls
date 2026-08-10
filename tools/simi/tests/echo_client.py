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

Usage: python3 echo_client.py /path/to/serial.sock

Exit code 0 = every marker observed; 1 = not.
"""
import socket
import sys
import time


def recv_until(s, buf, marker, timeout):
    """Read until marker appears in the accumulated stream (or timeout)."""
    deadline = time.time() + timeout
    while marker not in buf and time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break
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
    print(text[-1100:])
    ok = (b"[IRQ#1]" in buf) and (b"[IRQ#2]" in buf) and (b"[IRQ#3]" in buf) and \
         (b'unknown command: "ABC"' in buf) and \
         (b"[HELP] commands:" in buf) and \
         (b'unknown command: "PINX"' in buf) and \
         (b"[ECHO] hello world" in buf)
    if ok:
        print("ECHO_OK: interrupt tripwire pinned (1 keystroke -> 1 IRQ "
              "x3), help dispatched, backspace editing proven (PING\\bX "
              "-> PINX), CR-only line routed, echo <text> works")
    else:
        print("FAIL: interrupt-tripwire / readline / dispatch markers not all observed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

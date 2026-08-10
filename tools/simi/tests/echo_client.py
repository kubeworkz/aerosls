#!/usr/bin/env python3
"""echo_client.py — Phase 9i (ISA doc §16): the client half of the real
kernel's device-driven UART RX interrupt echo check (see
run_riscv_tests.sh's rv-uart-echo / rv-uart-echo-m blocks). QEMU boots
the echo kernel (sls_riscv_kernel_echo.elf or its M-mode twin) with the
16550 UART on a UNIX socket; this client drives the full readline +
command-loop protocol:

  1. wait for the "[UART] ECHO READY" banner (the PLIC wiring + SIE/MIE
     are live);
  2. "help\\r"            -> the [HELP] command list (dispatch works);
  3. "PING\\bX\\r"         -> each character echoed as typed, the backspace
     erases G (so the line is PINX), and the CR-only terminator routes
     it to dispatch, which reports an unknown command (proves char echo,
     backspace editing, CR-only line endings, and dispatch in one shot);
  4. "echo hello world\\r" -> [ECHO] hello world (the argument subcommand);
  5. "exit\\r"             -> S-mode powers the machine off via SBI_SRST
     (the socket sees EOF); the M-mode twin reports a halt instead.

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
    buf = recv_until(s, buf, b"ECHO READY", 10)
    if b"ECHO READY" not in buf:
        print("FAIL: kernel ECHO READY banner never seen")
        print(buf.decode(errors="replace"))
        return 1

    s.sendall(b"help\r")
    buf = recv_until(s, buf, b"[HELP]", 5)

    s.sendall(b"PING\bX\r")       # backspace erases the G; CR-only terminator
    buf = recv_until(s, buf, b"unknown command", 5)

    s.sendall(b"echo hello world\r")
    buf = recv_until(s, buf, b"[ECHO] hello world", 5)

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
    print(text[-900:])
    ok = (b"[HELP] commands:" in buf) and \
         (b'unknown command: "PINX"' in buf) and \
         (b"[ECHO] hello world" in buf)
    if ok:
        print("ECHO_OK: help dispatched, backspace editing proven "
              "(PING\\bX -> PINX), CR-only line routed, echo <text> works")
    else:
        print("FAIL: readline/dispatch markers not all observed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

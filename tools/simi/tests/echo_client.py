#!/usr/bin/env python3
"""echo_client.py — Phase 9i (ISA doc §16): the client half of the real
kernel's device-driven UART RX interrupt echo check (see
run_riscv_tests.sh's rv-uart-echo block). QEMU boots the echo kernel
(sls_riscv_kernel_echo.elf) with the 16550 UART on a UNIX socket; this
client connects, waits for the kernel's "[UART] ECHO READY" banner (which
proves the PLIC wiring + SIE are live), sends a "PING" line, and reports
whether the characters came back and the complete line reached the
headless shell — the first device-driven kernel I/O verified end to end.

Usage: python3 echo_client.py /path/to/serial.sock

Exit code 0 = the echo and the shell line were both observed; 1 = not.
"""
import socket
import sys
import time


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
    # The OpenSBI banner and kernel banner precede ECHO READY in the
    # stream; wait for the readiness marker before sending anything.
    deadline = time.time() + 10
    while b"ECHO READY" not in buf and time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break
    if b"ECHO READY" not in buf:
        print("FAIL: kernel ECHO READY banner never seen")
        print(buf.decode(errors="replace"))
        return 1

    s.sendall(b"PING\n")
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break

    tail = buf.decode(errors="replace")
    ok = (b"PING" in buf) and (
        b'no SLS shell on the RISC-V port; received: "PING"' in buf)
    print(tail[-600:])
    if ok:
        print("ECHO_OK: characters echoed and line reached the shell")
    else:
        print("FAIL: echo or shell line not observed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

# M2b — the loader's stdio ioctl

The two-DSO probe's last refusal was `ioctl` (16) → ENOSYS at the first
stdout write. The request was exactly the one musl makes before it: fd 1,
`TIOCGWINSZ` (0x5413). `__stdout_write` asks for the window size once to
decide whether stdout is line-buffered; it only checks the return and never
reads the `winsize`, so the console can answer 0 without a copy-out.

## Change

- `user/sidecar/src/linux.rs`: the ioctl arm (16). `TIOCGWINSZ` and
  `TCGETS` answer 0 on the console descriptors (fds 0..2, the same three
  every task is born with); any other request on a console fd is EINVAL,
  and a slot outside the table is EBADF. A valid non-console fd is a VFS
  file, whose ioctl the VFS does not serve — EINVAL, not ENOSYS.
- The reply log line now names the request: `ok ioctl fd=1 req=0x5413`.
- A unit test drives the three outcomes; guard clause **Y13** pins the arm
  and tooth **t154** (`ioctl-winsize-number-drift`) reddens it by name.
- Kernel and protocol are unchanged: ioctl was already forwarded; only the
  sidecar's answer changed.

## Run

- ISO: `m2b_ioctl.iso`, sha256 `5562314f2d1f114b769deed7a9ccf335e4433fd69a60794f95e0a296a31b23bf`
  (quiet gate profile: `E3_INIT_FEATURES=e3_envs,e7_gate M2A_INTERP=1`).
- `timeout 70 qemu-system-x86_64 -cdrom m2b_ioctl.iso -m 4G -smp 4
  -accel tcg -display none -monitor none -no-reboot -boot d
  -serial file:...` (exit 124 = the timeout).
- `boot.log` is the full serial log (2330 lines); `seam.txt` the child's
  whole trace.

## The two-DSO probe's trace — no refusals left

```
[E7] linux: num=0000009E child=m2a.linux.hello.0 ok             arch_prctl
[E7] linux: num=000000DA child=m2a.linux.hello.0 ok             set_tid_address
[E7] linux: num=0000000C child=m2a.linux.hello.0 ok             brk
[E7] linux: num=0000000C child=m2a.linux.hello.0 ok             brk
[E7] linux: num=00000009 child=m2a.linux.hello.0 ok mmap -> 0x0000400000016000
[E7] linux: num=0000000A child=m2a.linux.hello.0 ok             mprotect
[E7] linux: num=0000000A child=m2a.linux.hello.0 ok             mprotect
[E7] linux: num=00000010 child=m2a.linux.hello.0 ok ioctl fd=1 req=0x0000000000005413
[E7] linux: num=00000014 child=m2a.linux.hello.0 write ok len=7 |M2A-OK.|
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=0
[E7] linux: num=000000E7 child=m2a.linux.hello.0 group swept=1 dead=1 live=0
```

`grep 'm2a.linux.hello.0 ->' boot.log` is empty: the probe's path no longer
has a refusal of any kind, and the runner exits 0 with its group swept.

## Checks

- `tests/linux_personality_check.sh`: all clauses hold, Y13 included.
- `tests/linux_personality_check_smoke.sh`: 144 passed / 0 failed, with
  `TOOTH OK ioctl-winsize-number-drift — guard exited 1 naming Y13`.
- `cargo test -p aerosls-sidecar --lib`: 103 passed (the new ioctl test
  included).
- Kernel + sidecar image built clean (`make x86-iso-e3 ... BUILD_EXIT=0`).
- `user/sidecar/fixtures/m2a_hello.elf` unchanged:
  `be63cd4d1e5e4d4b9b4028cb4ec830a660fa19a6df1cb6fa27e6aaddd5deafcc`.

## Correction (later commit)

TCGETS was answered 0 in the run above without writing a termios. It is now
ENOTTY (CAP_ENOTTY -35 → Linux 25): the console is not a tty, and a success
would promise bytes this side never writes. TIOCGWINSZ keeps its 0 answer —
that is the request this run actually issued (`req=0x5413`), and musl never
reads the winsize. No probe rerun is needed: the loader never asked TCGETS.

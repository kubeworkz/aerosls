# M2b — the mprotect ledger's loaded-image span

The loader's RELRO pass was the M2b stall: after the four file maps were
served, it called `mprotect(0x7f00000b2000, 4096, PROT_READ)` over its own
image and `mprotect(0x403000, 4096, PROT_READ)` over the program's, and the
sidecar's twin answered `-EINVAL`. The kernel had already admitted both — its
`linux_mprotect_span()` is the heap stamp united with the loaded images — but
the ledger was born with the manifest's brk span alone, and the interpreter's
half of the image span is the kernel's placement, not something the manifest
carries.

## Change

- `SYS_SLS_LINUX_IMAGE_SPAN` (336): `kernel/cap.h` request struct,
  `kernel/linux_clone.c` handler (sidecar authority, Linux personality and
  partition checked; returns the task's stamped `linux_image_lo/hi`),
  `kernel/syscall_dispatch.c` arm.
- `user/proto/src/kabi.rs`: the request twin, `k_linux_image_span`, the
  `Kernel::linux_image_span` method and the `RealKernel` implementation.
- `user/sidecar/src/linux.rs`: the group ledger gains `image_lo/image_hi`
  (through `MemBrk`), `track`/`track_mirrored` take the span, and the
  mprotect twin spans `min(brk.base, image_lo) .. max(brk.limit, image_hi)`
  — the kernel's own arithmetic. `entry.rs` asks the kernel at every spawn
  and passes the answer in (`image_span(pid)`).
- Guard clause Y12 pins the chain; smoke tooth t153 (`t153`) drops the image
  half from the ledger and expects Y12 by name.

## Run

- ISO: `m2b_span.iso`, sha256 `42143a477faa44c7a640be1539fe98e91ac76239ae64ee4319bdc60ec52f3812`
  (quiet gate profile: `E3_INIT_FEATURES=e3_envs,e7_gate M2A_INTERP=1`).
- `timeout 70 qemu-system-x86_64 -cdrom m2b_span.iso -m 4G -smp 4
  -accel tcg -display none -monitor none -no-reboot -boot d
  -serial file:...` (exit 124 = the timeout, not a failure).
- `boot.log` is the full serial log (2398 lines); `seam.txt` is the
  extracted seam.

## Result — the probe completes, no next break at this seam

```
[E7] linux mprotect apply addr=0x7f00000b2000 len=4096 prot=0x1 reply=0x0 (span 0x400000..0x7f00000b7000)
[E7] mprotect addr=0x7f00000b2000 len=4096 prot=0x1: 1 page(s) re-protected
[E7] linux mprotect apply addr=0x403000 len=4096 prot=0x1 reply=0x0 (span 0x400000..0x7f00000b7000)
[E7] mprotect addr=0x403000 len=4096 prot=0x1: 1 page(s) re-protected
...
[E7] linux: num=00000014 child=m2a.linux.hello.0 write ok len=7 |M2A-OK.|
M2A-OK
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=0
[E7] linux: num=000000E7 child=m2a.linux.hello.0 group swept=1 dead=1 live=0
```

The loader's RELRO now reaches the kernel, the program runs, and the
two-DSO probe exits 0. There is no further refusal on the probe's path.

The only syscall the child sees refused afterwards is `ioctl` (16) →
ENOSYS at `boot.log:620`; musl proceeds without it and the marker still
prints, so it is a named gap, not this slice's break. The log ends at the
gate profile's deliberate copy-continuation stop (`[E7-GATE] held at the
copy continuation`), which is the instrument, not a stall.

## Checks

- `tests/linux_personality_check.sh`: all clauses hold, Y12 included.
- `tests/linux_personality_check_smoke.sh`: 143 passed / 0 failed, with
  `TOOTH OK image-span-dropped-from-ledger — guard exited 1 naming Y12`.
- `cargo test -p aerosls-sidecar --lib`: 102 passed.
- Kernel + sidecar image built clean (`make x86-iso-e3 ... BUILD_EXIT=0`).
- `user/sidecar/fixtures/m2a_hello.elf` unchanged:
  `be63cd4d1e5e4d4b9b4028cb4ec830a660fa19a6df1cb6fa27e6aaddd5deafcc`.

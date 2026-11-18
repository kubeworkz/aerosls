# M2b fstat(5) — the loader reads `libm2a.so`'s stat

Date: 2026-11-18
Branch: `m1/linux-network-bridge`
Base: `8d57541`

> Follow-up: this run's file-backed mmap refusal is where the next slice
> starts.

## Goal

The library-namespace run made `open("/lib/libm2a.so")` succeed, but the
loader's next call, `fstat(5)`, had no arm and answered ENOSYS. This slice
serves the descriptor's stat so the loader can learn the DSO's size and
mode.

## What changed

- `user/sidecar/src/linux.rs`: `LINUX_FSTAT = 5`; an `answer_file` arm that
  answers an fd-class file through `Vfs::fstat` and a console through
  `CharNode::stat`, both as the same compact 24-byte Stat
  (`stat_answer`). `payload_bound` accepts exactly that payload at
  `args[1]`, and `service` routes 5 into the file class. Guard Y8 pins this
  whole seam; smoke t142–t144 mutate its three layers.
- `kernel/linux_abi.h` / `linux_abi.c`: `LINUX_FSTAT` and its `LSC_FD`
  table row; `linux_fd_reply_plan` stages `args[1]` with the existing
  `LINUX_STAGE_STAT` serializer (24 compact bytes → musl's 144-byte
  `struct stat`); `linux_reply_shape_ok` accepts the stat shape.

## Live result: fstat succeeds; file-backed mmap is the next break

```
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /etc/ld-musl-x86_64.path
[E7] linux: num=00000002 child=m2a.linux.hello.0 ok
[E7] linux: num=00000048 child=m2a.linux.hello.0 ok
[E7] linux: num=00000005 child=m2a.linux.hello.0 ok + payload
[E7] linux: num=00000000 child=m2a.linux.hello.0 ok + payload
[E7] linux mmap addr=0x0 len=20480 prot=1 flags=0x2 refused before post: shape not served (by name)
[E7] linux: num=00000003 child=m2a.linux.hello.0 ok
...
Error loading shared library libm2a.so: Function not implemented
Error relocating m2a.linux.hello.0: m2a_add: symbol not found
Error relocating m2a.linux.hello.0: m2a_data: symbol not found
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=127
```

`fstat(5)` now carries the compact Stat back, the kernel serializes it, and
the loader reads the ELF header (`read(0) ok + payload`). It then asks to
map the DSO from the file: `len=20480`, `prot=1` (PROT_READ), `flags=0x2`
(MAP_PRIVATE), with no MAP_ANONYMOUS. The kernel's mmap ladder refuses the
shape before any post; the loader closes the fd and prints "Function not
implemented", exiting 127.

## Next break

File-backed `mmap` is not just a flag to relax: `struct LinuxMmapReq`
carries only `addr/len/prot/flags` — no fd and no offset — so neither the
kernel plan nor the sidecar's mmap arm has the words needed to map the
file's pages. Supporting it needs a longer request shape (fd + offset), a
VFS backing for the pages, and the existing anonymous ladder kept
untouched. `M2B-OK` still does not print.

## Checks

- Guard: `all clauses hold` (134 ok, with Y8).
- Smoke: `134 passed, 0 failed` — t142 (table row), t143 (sidecar number)
  and t144 (reply staging) each redden Y8 by name.
- `cargo check` passes with `target,m2a_interp`.
- M2a fixture restored after the probe:
  `be63cd4d1e5e4d4b9b4028cb4ec830a660fa19a6df1cb6fa27e6aaddd5deafcc`.

## Artifacts

- `boot.log` — full serial log of the run.
- `markers.txt` — the fstat/read/mmap/loader/exit lines above.
- `facts` — the same facts in key=value form.

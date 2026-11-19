# M2b — file-backed mmap (fd/offset) — probe evidence, 2026-11-19

Build: `make x86-iso-e3 M2A_INTERP=1 X86_E3_ISO=m2b_filemap.iso
SIDECAR_E3_CPIO=m2b_filemap.cpio` (iso.sha256 here).
Boot: `timeout 60 qemu-system-x86_64 -cdrom m2b_filemap.iso -m 4G -smp 4
-accel tcg -display none -monitor none -no-reboot -boot d -serial file:…`
(serial log is boot.log; the QEMU exit 124 is the timeout, not a crash).

Probe: the committed two-DSO fixture (`m2a_hello.elf` = `/tmp/m2b/m2b_hello.elf`,
sha 6518da9f…), loader `/lib/ld-musl-x86_64.so.1`, DSO `/lib/libm2a.so`
(13800 bytes, sha 398e7962…).

## What now works

The loader's first DSO map is served from the file:

```
[E7] linux: num=00000009 child=m2a.linux.hello.0 ok mmap -> 0x0000400000051000 fd=3 off=0x00000
[E7] linux: num=4D4D4650 child=m2a.linux.hello.0 ok + payload   (×6)
[E7] linux: num=0000000B child=m2a.linux.hello.0 ok munmap addr=0x0000400000051000 len=20480
```

- Kernel: `LinuxMmapReq` carries `fd`/`offset` (args[4]/args[5]); the ladder
  serves `MAP_PRIVATE`, non-anonymous, `PROT_READ`, page-aligned offset,
  `fd <= INT32_MAX`, `len <= 32768`; verdict `file_backed`.
- The grant is applied first (five zero-filled pages), then the apply posts
  bounded reads of the file at the staged fd/offset — request number
  `LINUX_MMAP_FILL` (0x4D4D4650), 4024 bytes per round (one full reply
  payload), 6 rounds for 20 KiB — and `LINUX_STAGE_FILL` writes each round
  into the already-mapped frame (the map is read-only to ring 3, so the
  generic user-copy's WRITE predicate is the wrong one). The syscall returns
  only after the last round lands, so ring 3 never observes a partially
  backed mapping.
- Sidecar: `Answer::MmapFile` serves the eligibility/ledger; the FILL arm
  reads via the new `Vfs::read_at` and zero-pads to the round length, so past
  EOF a round still returns `ret == n`.

## Next break

After filling and unmapping the 20 KiB map, the loader maps the DSO's
`.text` segment at its fixed address:

```
[E7] linux mmap addr=0x400000052000 len=4096 prot=5 flags=0x12 fd=3 off=0x1000
refused before post: shape not served (by name)
[E7] linux: num=0000000E … write |Error loading shared lib|
[E7] linux: num=0000000E … write |.|
Error loading shared library libm2a.so: Function not implemented
… exit_group status=127
```

`prot=5` is `PROT_READ|PROT_EXEC`, `flags=0x12` is `MAP_PRIVATE|MAP_FIXED`,
`off=0x1000`: the first PT_LOAD's executable segment, re-mapped MAP_FIXED
over the region just released. The file branch refuses anything but
`PROT_READ`, so the next slice is RX file maps: serve `READ|EXEC` (and
`MAP_FIXED` at the stamped span), map the pages executable, and keep the
fill rounds (kernel writes the frames, so exec-only mapping is not a
problem for the fill itself).

`M2A-OK` is untouched (`m2a_hello.elf` was restored to sha be63cd4d… before
the guard run); `M2B-OK` is still unreachable.

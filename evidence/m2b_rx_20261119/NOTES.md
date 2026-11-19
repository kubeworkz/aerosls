# M2b — executable file maps (the DSO's .text) — 2026-11-19

Build: `make x86-iso-e3 M2A_INTERP=1 X86_E3_ISO=m2b_rx.iso
SIDECAR_E3_CPIO=m2b_rx.cpio` (iso.sha256 here).
Boot: `timeout 60 qemu-system-x86_64 -cdrom m2b_rx.iso -m 4G -smp 4
-accel tcg -display none -monitor none -no-reboot -boot d -serial file:…`
(boot.log; QEMU exit 124 is the timeout).

Probe: the committed two-DSO fixture (`m2a_hello.elf` = `/tmp/m2b/m2b_hello.elf`,
sha 6518da9f…), loader `/lib/ld-musl-x86_64.so.1`, DSO `/lib/libm2a.so`
(13800 bytes, sha 398e7962…). DSO program headers: LOAD R @0x0, LOAD R E
@0x1000 (16 bytes), LOAD R @0x2000 (76 bytes), LOAD RW @0x2ee0 (292 bytes).

## What now works

The loader re-maps each segment at its fixed address, and all three file
maps are served with their fill rounds:

```
[E7] file mmap addr=0x0          len=20480 prot=1 flags=0x2  fd=3 off=0x0
[E7] linux: num=00000009 … ok mmap -> 0x0000400000051000 fd=3 off=…   (6 rounds)
[E7] file mmap addr=0x400000052000 len=4096 prot=5 flags=0x12 fd=3 off=0x1000
[E7] linux: num=00000009 … ok mmap -> 0x0000400000052000 fd=3 off=…   (2 rounds)
[E7] file mmap addr=0x400000053000 len=4096 prot=1 flags=0x12 fd=3 off=0x2000
[E7] linux: num=00000009 … ok mmap -> 0x0000400000053000 fd=3 off=…   (2 rounds)
[E7] linux: num=0000000B … ok munmap addr=0x0000400000051000 len=20480
```

- Ladder: a file map may be `PROT_READ` or `PROT_READ|PROT_EXEC` — never
  writable, so §5.4's W^X holds by the shape alone. An ANONYMOUS executable
  map stays ENOSYS by name.
- Verdict: `prot_exec` for a file map with the exec bit; `prot_write` stays 0.
  The apply installs the PTE without `USER_PTE_NOEXEC` exactly there.
- Sidecar twin: the file arm admits `prot == 0x1 || prot == 0x5`.
- Fill rounds are unchanged (kernel writes the frames; NX is irrelevant to
  the fill itself). New unit test `file_maps_carry_the_requested_fd_and_offset`
  pins the answer's VFS fd, file offset and length, and that a writable file
  map is still refused.

Note on log width: the sidecar's grant line lives in a 96-byte buffer and
`hex_into` prints 16 digits, so the tail of the line is clipped — `off=0x1000`
shows as `off=0x00000`. The kernel's `[E7] file mmap …` line carries the true
request words.

## Next break

The DSO's data segment:

```
[E7] linux mmap addr=0x400000054000 len=8192 prot=3 flags=0x12 fd=3 off=0x2000
refused before post: shape not served (by name)
Error loading shared library libm2a.so: Function not implemented
… exit_group status=127
```

`prot=3` is `PROT_READ|PROT_WRITE`, `flags=0x12` is `MAP_PRIVATE|MAP_FIXED`,
`len=8192`, `off=0x2000`: the loader maps the page-aligned window covering
`.data` (p_offset 0x2ee0, p_vaddr 0x3ee0) through the page past it — the
last page lies beyond the 13800-byte file, so its bytes are the sidecar's
zero padding. The next slice is RW file maps: admit `PROT_READ|PROT_WRITE`
for a file map, set `prot_write` so the PTE is writable, keep the fill
rounds, and let the loader relocate.

`M2A-OK` is untouched (`m2a_hello.elf` restored to sha be63cd4d…);
`M2B-OK` is still unreachable.

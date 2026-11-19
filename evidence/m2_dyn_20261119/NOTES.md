# M2 dynamic spawn — arbitrary PT_INTERP hello from the rootfs (2026-11-19)

The M2a pair proved the kernel's version-2/INTERP path with one committed,
fixture-specific spawn. This slice removes the fixture from the spawn site:
the caller names a **VFS path**, and the spawner reads the image there, parses
its `PT_INTERP`, reads the loader that path names out of the rootfs `/lib`
namespace, and packs the version-2 manifest itself. A new, independent image
proves the route.

## What changed

- `user/sidecar/src/m2_dyn.rs` — `spawn(k, vfs, image_path, name, budget…)`:
  reads the image via `Vfs::open`/`read_at` into the budget tail, parses
  `PT_INTERP` from the staged bytes, reads **that** loader path through the
  same VFS, stages both page-aligned, packs version 2.
- `boot.rs` — the `/lib` ramfs now seeds the loader
  (`/lib/ld-musl-x86_64.so.1`) beside `libm2a.so`, and the arbitrary hello is
  seeded as an ordinary rootfs payload at `/tmp/m2.dyn.hello`.
- `entry.rs` — after the M2a one-shot, the same boot-generation block calls
  `m2_dyn::spawn` with that path and tracks the child.
- `kernel/cap.c` — a version-1 manifest carrying an image with `PT_INTERP` is
  now refused by name (`CAP_EINVAL: PT_INTERP requires a version-2 manifest`)
  instead of being entered at `e_entry` with no loader.
- `user/sidecar/fixtures/m2_dyn_hello.{S,elf}` + `build_m2_dyn.sh` — an
  ET_EXEC built with plain gcc, `-nostdlib`, raw `write`/`exit` syscalls:
  no musl object is shared with M2a; `PT_INTERP /lib/ld-musl-x86_64.so.1`,
  `DT_NEEDED libc.so` (satisfied by the loader itself). It prints
  `M2DYN-OK` only if the loader transferred control.

Fixture SHA-256: `e12e4d2e…` (raw syscall image, 0x3468 bytes).

## Build / boot

```sh
make x86-iso-e3 M2A_INTERP=1 E3_INIT_FEATURES=e3_envs,e7_gate \
     X86_E3_ISO=/tmp/m2dyn.iso SIDECAR_E3_CPIO=/tmp/m2dyn.cpio
timeout 70 qemu-system-x86_64 -cdrom /tmp/m2dyn.iso -m 4G -smp 4 \
     -accel tcg -display none -monitor none -no-reboot -boot d \
     -serial file:/tmp/m2dyn_boot.log
```

ISO SHA-256: `b4ef5284…` (19816448 bytes).

## Live trace

```text
[M2D] staged image=00003468
[M2D] staged loader=000B29E0
[M2D] hello: spawned ch_00000015
[CS] dynamic image: main entry=0x401000 interp base=0x7f0000000000 \
     entry=0x7f000007bf0e path='/lib/ld-musl-x86_64.so.1'
[E7] linux: num=0000009E child=m2.dyn.hello.0 ok
[E7] linux: num=0000000C child=m2.dyn.hello.0 ok        (brk)
[E7] linux: num=0000000C child=m2.dyn.hello.0 ok
[E7] linux: num=00000009 child=m2.dyn.hello.0 ok \
     mmap -> 0x0000400000015000
[E7] linux: num=0000000A child=m2.dyn.hello.0 ok        (mprotect)
[E7] linux: num=0000000A child=m2.dyn.hello.0 ok
[E7] linux: num=00000001 child=m2.dyn.hello.0 write ok len=9 |M2DYN-OK.|
[E7] linux: num=0000003C child=m2.dyn.hello.0 exit status=0
```

The M2a child still runs in the same boot (`M2A-OK`, write len=7), so the
generic route did not replace or disturb the fixture route. 0 refusals on
either dynamic child; no `CAP_EINVAL`, no spawn/track failure.

## Next break

The loader now has a namespace and the dynamic image starts from a rootfs
path, but nothing on this route yet loads a **second shared object**: this
hello's only `DT_NEEDED` is `libc.so`, which musl's loader satisfies from
itself. The next slice is the DSO path (`DT_NEEDED libm2a.so`, symbol
resolution, the loader's own `mmap`/RELRO over the DSO), which the M2b
two-DSO probe last stopped at with `m2a_add: symbol not found` before the
`/lib` namespace existed. That binary is not committed and this machine has
no musl compiler, so it must be rebuilt before the probe can be re-run.

Not exercised live: the new version-1 `PT_INTERP` refusal (code + guard O10
only; the v2 path is what ran).

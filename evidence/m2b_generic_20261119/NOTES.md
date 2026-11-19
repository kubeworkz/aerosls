# M2b two-DSO probe through the generic spawn (2026-11-19)

The M2b probe — a non-PIE musl hello with `DT_NEEDED libm2a.so` and
`libc.so` — had only ever run through the older M2a spawn site. Its built
binary was never committed and WSL had no musl toolchain, so this slice was
blocked. This run rebuilds `m2b_hello.elf`, stages it as the image-agnostic
route's rootfs payload, and boots it.

**Result: `M2B-OK`, exit 0. There is no next break on this path.** The
generic spawn reads the image from the VFS, parses its `PT_INTERP`, reads
the named loader from `/lib`, and the loader finds the seeded
`/lib/libm2a.so`, maps all four DSO segments, relocates `m2a_add` and
`m2a_data`, completes RELRO, and runs the program.

## Toolchain

Ubuntu noble's musl 1.2.4 packages were downloaded and extracted into a
local prefix; no root install and no sudo:

```sh
apt-get download musl musl-dev musl-tools
mkdir -p "$HOME/musl-root"
dpkg-deb -x musl_1.2.4-2_amd64.deb "$HOME/musl-root"
dpkg-deb -x musl-dev_1.2.4-2_amd64.deb "$HOME/musl-root"
dpkg-deb -x musl-tools_1.2.4-2_amd64.deb "$HOME/musl-root"

sed "s#/usr/#$HOME/musl-root/usr/#g" \
    "$HOME/musl-root/usr/lib/x86_64-linux-musl/musl-gcc.specs" \
    > "$HOME/musl-root/usr/lib/x86_64-linux-musl/musl-gcc-local.specs"

cat > "$HOME/musl-gcc-local" <<'EOF'
#!/bin/sh
exec gcc "$@" -specs "$HOME/musl-root/usr/lib/x86_64-linux-musl/musl-gcc-local.specs"
EOF
chmod +x "$HOME/musl-gcc-local"
```

(The installed wrapper uses a fixed specs path; the local copy above is what
the build below names.)

## Build

`build_m2b.sh` here is the probe's original recipe, unchanged apart from a
comment recording the local toolchain invocation:

```sh
MUSL_CC="$HOME/musl-gcc-local" \
MUSL_CRT1="$HOME/musl-root/usr/lib/x86_64-linux-musl/crt1.o" \
M2B_OUT=/tmp/m2bout sh build_m2b.sh
```

It reproduced the original evidence bytes exactly:

```text
6518da9f57ed24b415aba62e9b65bb95d3e6256ce5bd42df4daefdb87792885f  m2b_hello.elf
398e79629f49179d8225f18a5b8216ef444cc57408dd6e770d7dc53366eecf6d  libm2a.so
```

The rebuilt DSO is byte-identical to the committed
`user/sidecar/fixtures/libm2a.so`, so the boot used the existing pinned
fixture rather than a fresh copy.

The main image is 16616 bytes, `ET_EXEC`, entry `0x4010a6`,
`PT_INTERP /lib/ld-musl-x86_64.so.1`, and needs `libm2a.so` then `libc.so`.
It carries a `COPY` relocation for `m2a_data` and a `JUMP_SLOT` for
`m2a_add`; the DSO's own `m2a_data` access is a `GLOB_DAT`.

## Probe staging and run

The generic spawn names `/tmp/m2.dyn.hello` and knows nothing about the
bytes, so the probe temporarily replaced the M2D seed with the M2b image,
built the ISO, booted it, and restored the fixture:

```sh
cp user/sidecar/fixtures/m2_dyn_hello.elf /tmp/m2_dyn_hello.elf.orig
cp /tmp/m2bout/m2b_hello.elf user/sidecar/fixtures/m2_dyn_hello.elf

make x86-iso-e3 M2A_INTERP=1 E3_INIT_FEATURES=e3_envs,e7_gate \
     X86_E3_ISO=/tmp/m2b_generic.iso SIDECAR_E3_CPIO=/tmp/m2b_generic.cpio

timeout 70 qemu-system-x86_64 -cdrom /tmp/m2b_generic.iso -m 4G -smp 4 \
     -accel tcg -display none -monitor none -no-reboot -boot d \
     -serial file:/tmp/m2b_generic_boot.log

cp /tmp/m2_dyn_hello.elf.orig user/sidecar/fixtures/m2_dyn_hello.elf
```

ISO SHA-256: `42d7e5397ee2c0aab5c72ee4ac423b5ed9cd805d5543e6be17b90ea4f37dcba3`
(`m2b_generic.iso`, 19820544 bytes). The boot log is 1213 lines.

The M2D fixture is restored in the committed tree at its original SHA-256
`e12e4d2e…`; the M2a pair and loader are untouched at `be63cd4d…` and
`6e8ff8ad…`.

## Live trace

```text
[M2D] staged image=000040E8                 <- the 16616-byte two-DSO image
[M2D] staged loader=000B29E0
[M2D] hello: spawned ch_00000015
[CS] CAP_EINVAL: PT_INTERP requires a version-2 manifest (version=1, path='/lib/ld-musl-x86_64.so.1')
[M2D] v1 CAP_EINVAL rc=FFFFFFFF             <- the v1 negative probe still holds

[E7] linux: num=00000002 child=m2.dyn.hello.0 -> ENOENT open /etc/ld-musl-x86_64.path
[E7] linux: num=00000002 child=m2.dyn.hello.0 ok
[E7] linux: num=00000048 child=m2.dyn.hello.0 ok
[E7] linux: num=00000005 child=m2.dyn.hello.0 ok + payload
[E7] linux: num=00000000 child=m2.dyn.hello.0 ok + payload

[E7] file mmap addr=0x0 len=20480 prot=1 flags=0x2 fd=3 off=0x0
[E7] linux: num=00000009 child=m2.dyn.hello.0 ok mmap -> 0x0000400000051000 fd=3 off=0x0
... 6 fill rounds ...
[E7] file mmap addr=0x400000052000 len=4096 prot=5 flags=0x12 fd=3 off=0x1000
[E7] linux: num=00000009 child=m2.dyn.hello.0 ok mmap -> 0x0000400000052000 fd=3 off=0x0
[E7] file mmap addr=0x400000053000 len=4096 prot=1 flags=0x12 fd=3 off=0x2000
[E7] linux: num=00000009 child=m2.dyn.hello.0 ok mmap -> 0x0000400000053000 fd=3 off=0x0
[E7] file mmap addr=0x400000054000 len=8192 prot=3 flags=0x12 fd=3 off=0x2000
[E7] linux: num=00000009 child=m2.dyn.hello.0 ok mmap -> 0x0000400000054000 fd=3 off=0x0
... 13 fill rounds total ...
[E7] linux: num=00000003 child=m2.dyn.hello.0 ok          <- close(3)

[E7] linux mprotect apply addr=0x400000054000 len=4096 prot=0x1 reply=0x0
[E7] linux mprotect apply addr=0x7f00000b2000 len=4096 prot=0x1 reply=0x0
[E7] linux mprotect apply addr=0x403000 len=4096 prot=0x1 reply=0x0

[E7] linux: num=00000010 child=m2.dyn.hello.0 ok ioctl fd=1 req=0x0000000000005413
[E7] linux: num=00000014 child=m2.dyn.hello.0 write ok len=7 |M2B-OK.|
M2B-OK
[E7] linux: num=000000E7 child=m2.dyn.hello.0 exit_group status=0
[E7] linux: num=000000E7 child=m2.dyn.hello.0 group swept=1 dead=1 live=0
```

The same boot still runs the M2a route: `M2A-OK` is written and the child
exits 0. `M2DYN-OK` is absent only because the generic spawn's seed was
temporarily the M2b image; the restored fixture is what the M2D boot uses.

## Next break

**None on the two-DSO path.** The loader found the DSO in the seeded `/lib`
ramfs, served all four file-backed maps (20 KiB read-only, 4 KiB RX, 4 KiB
R, 8 KiB RW), completed the fill rounds and RELRO mprotects, resolved both
the copy relocation and the jump slot, and the program printed `M2B-OK` and
exited 0.

The refusals elsewhere in the log are off this path: W+X anonymous `mmap`
refusals in the `e7` backup applet, `ENOMEM` for 1 MiB `mmap`s in the
`e7.linux.probe`/`cand` tenants, and two `munmap` refusals at
`0x400000006000`/`0x400000007000` belonging to `e7.linux.user`, outside the
M2b child's span.

## Checks

- ISO build: exit 0, SHA-256 above.
- Boot: `M2B-OK`, `exit_group status=0`, group sweep `dead=1 live=0`.
- M2a on the same boot: `M2A-OK`, exit 0.
- M2d-v1 on the same boot: named `CAP_EINVAL`, rc `FFFFFFFF`.
- Committed fixtures after restore:
  `m2_dyn_hello.elf e12e4d2e…`, `m2a_hello.elf be63cd4d…`,
  `ld-musl-x86_64.so.1 6e8ff8ad…`, `libm2a.so 398e7962…`.
- Guard and smoke were not rerun for this probe; it changes no source and
  no fixture.

## Artifacts

- `m2b_hello.elf` — the rebuilt 16616-byte image (`6518da9f…`).
- `m2b_hello.c`, `libm2a.c`, `build_m2b.sh` — source and build recipe.
- `boot.log` — full 1213-line serial log.
- `seam.txt` — the M2b/M2D lines extracted from it.
- `iso.sha256` — the ISO hash.
- `files.sha256` — artifact and fixture hashes.

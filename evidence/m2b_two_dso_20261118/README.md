# M2b two-DSO probe (2026-11-18)

This slice asks the existing M2a loader path to load a **second** shared
object. `m2b_hello.elf` has `DT_NEEDED libm2a.so` and `libc.so`, calls
`m2a_add` across the library boundary, and reads `m2a_data` through the
executable's copy relocation. `libm2a.so` is a PIC DSO with one exported
data symbol and one function; its own access to `m2a_data` is a GOT data
relocation.

**Result: the two-DSO hello does not run.** The live boot proves where the
current path stops: musl's loader resolves the first non-libc `DT_NEEDED`,
tries the path file and the three default library directories, gets
`ENOENT` for each, then reports unresolved `m2a_add`/`m2a_data` and exits
127. `M2B-OK` never prints.

## Pair shape

```text
main:     NEEDED libm2a.so
          NEEDED libc.so
          R_X86_64_COPY      m2a_data
          R_X86_64_JUMP_SLOT m2a_add
libm2a:   R_X86_64_GLOB_DAT   m2a_data
```

`m2b_hello.c` computes `m2a_add(m2a_data)` and prints `M2B-OK` only when the
result is 82. The build recipe is the M2a one (musl `crt1.o`,
`-nostartfiles`, and the committed loader used as `libc.so`), plus the DSO:

```sh
MUSL_CC=/tmp/musl-gcc-local \
MUSL_CRT1=/tmp/muslroot/usr/lib/x86_64-linux-musl/crt1.o \
REALGCC=gcc sh build_m2b.sh
```

The live probe swapped `user/sidecar/fixtures/m2a_hello.elf` for the built
main, rebuilt `make x86-iso-e3 M2A_INTERP=1`, booted it, then restored the
fixture. The restored hash is `be63cd4d…` (the Y5 pin).

## Live result

```text
[CS] dynamic image: main entry=0x4010a6 interp base=0x7f0000000000 entry=0x7f000007bf0e path='/lib/ld-musl-x86_64.so.1'
[M2A] hello: spawned ch_00000013
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /etc/ld-musl-x86_64.path
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /lib/libm2a.so
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /usr/local/lib/libm2a.so
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /usr/lib/libm2a.so
Error loading shared library libm2a.so: No such file or directory (needed by m2a.linux.hello.0)
Error relocating m2a.linux.hello.0: m2a_add: symbol not found
Error relocating m2a.linux.hello.0: m2a_data: symbol not found
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=127
```

Each failed open now names its errno and the path it tried. (The sidecar's
generic answer line previously printed `ok` for **every non-ENOSYS
status**, which is why the first run of this probe looked like four
successful opens.) No `read`, `close`, or mmap follows.

## Assumptions the current path breaks

1. **One INTERP is not a library set.** The manifest and kernel model carry
   exactly one `INTERP` record and validate exactly one `PT_INTERP`; there
   is no notion of a `DT_NEEDED` list. Only `libc.so` is special-cased (to
   the loader itself), so the second name falls into the loader's file
   search.
2. **There is no library namespace.** The M2a child has `max_fds: 0`,
   `max_open_files: 0`, and no file record in its manifest. The loader's
   four search-path opens are answered with an error (ENOENT), so no fd ever
   reaches the ELF header read.
3. **File-backed `mmap` is not served.** The kernel's mmap ladder requires
   `MAP_ANONYMOUS|MAP_PRIVATE` and refuses everything else with `-ENOSYS`.
   Even if `libm2a.so` were present in a VFS, musl maps DSO segments from an
   fd, so the loader still could not map it.
4. **The loader's libc shortcut is not a second-DSO loader.** `libc.so`
   resolves to the loader because musl is both; there is no mapped or
   relocated `libm2a.so`, and the loader reports its symbols as unresolved.
5. **The probe itself is still compile-time/demo-shaped.** The pair is
   embedded with `include_bytes!`, enabled by `M2A_INTERP=1`, one-shot via
   the boot-generation gate, and sized into a fixed 1 MiB reserve/256 KiB
   memory budget. Arbitrary tenant-supplied dynamic payloads are not on this
   path.
6. **Errno-accurate open logging was missing (fixed here).** The generic
   answer line printed `ok` for every non-ENOSYS status; it now prints
   `-> ENOENT open <path>`, so the failure names the path and error without
   reading the loader's stderr.

## What M2b needs next

- A manifest library/file record (or an equivalent sidecar VFS plant) so a
  child can see `/lib/libm2a.so`.
- A file-backed `mmap` arm (fd + offset + permissions) or a loader-specific
  pre-map/link-map injection that does not require one.
- Close logging is still generic; open now names the errno and the failed
  path (landed with this probe).

## Artifacts

- `boot.log` — full serial log of the probe boot.
- `markers.txt` — the key lines above.
- `facts` — compact result fields.
- `libm2a.c`, `m2b_hello.c`, `build_m2b.sh` — the probe pair and its build
  recipe (evidence only; not wired into the build).

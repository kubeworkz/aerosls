# M2b library namespace — `/lib/libm2a.so` served

Date: 2026-11-18
Branch: `m1/linux-network-bridge`
Base: `cb29ef4` (errno/path logging) plus this change

## Goal

The previous two-DSO run showed the loader's four `open` attempts all
returning ENOENT: the child's root aerofs image is a fixed, read-only
format with no `/lib/libm2a.so`. This slice gives the child a library
namespace so the loader's `DT_NEEDED` search finds the DSO.

## What changed

- `user/vfs/src/vfs.rs`: new `Vfs::add_ram_file(path, data, mode)` —
  bootstrap-owned namespace plumbing that resolves the path, creates the
  leaf in its parent, and writes the bytes (no task/fd/rights).
- `user/sidecar/src/boot.rs`: when the `m2a_interp` feature is on, mount a
  ramfs at `/lib` and seed it with the embedded probe library. The root
  aerofs mount is untouched.
- `user/sidecar/src/m2a_interp.rs`: embed `fixtures/libm2a.so`
  (`M2A_LIB`) and name `M2A_LIB_PATH = "/lib/libm2a.so"`.
- `user/sidecar/fixtures/libm2a.so`, `libm2a.c`, `build_m2b_lib.sh`:
  the probe's DSO, its source, and a reproducible rebuild. SHA-256
  `398e79629f49179d8225f18a5b8216ef444cc57408dd6e770d7dc53366eecf6d`.

No kernel or loader change: the open now succeeds through the same
M2a path.

## Live result: open succeeds; `fstat(5)` is the next break

Booted `m2b_libns.iso` (same probe main, `DT_NEEDED libc.so libm2a.so`):

```
[E7] linux: num=00000002 child=m2a.linux.hello.0 -> ENOENT open /etc/ld-musl-x86_64.path
[E7] linux: num=00000002 child=m2a.linux.hello.0 ok
[E7] linux: num=00000048 child=m2a.linux.hello.0 ok
[E7] linux: num=00000005 child=m2a.linux.hello.0 -> ENOSYS
[E7] linux: num=00000003 child=m2a.linux.hello.0 ok
...
Error loading shared library libm2a.so: Function not implemented
Error relocating m2a.linux.hello.0: m2a_add: symbol not found
Error relocating m2a.linux.hello.0: m2a_data: symbol not found
[E7] linux: num=000000E7 child=m2a.linux.hello.0 exit_group status=127
```

The loader's sequence is now readable by number: the optional path file
still ENOENTs, then `/lib/libm2a.so` opens (`open(2) -> ok`), `fcntl(72)`
succeeds, **`fstat(5)` answers ENOSYS**, and the loader closes (`close(3)`)
and gives up. "Function not implemented" is that ENOSYS; the relocations
are still unresolved because the DSO was never mapped.

## Next break

`fstat(5)` has no arm in the sidecar (and no kernel plan), so the loader
cannot read the DSO's size/mode. It never reaches the already-known
file-backed `mmap` refusal (both the kernel plan and the sidecar accept
only `MAP_ANONYMOUS|MAP_PRIVATE`), which remains the break after fstat.

`M2B-OK` still does not print.

## Checks

- Guard: `all clauses hold` (132 ok).
- Smoke: `129 passed, 0 failed`.
- `cargo check` passes for the sidecar with and without `m2a_interp`.
- M2a fixture restored after the probe:
  `be63cd4d1e5e4d4b9b4028cb4ec830a660fa19a6df1cb6fa27e6aaddd5deafcc`.

## Artifacts

- `boot.log` — full serial log of the run.
- `markers.txt` — the open/fstat/loader/exit lines above.
- `facts` — the same facts in key=value form.

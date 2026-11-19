# M2d-v1 — the version-1 PT_INTERP refusal, live (2026-11-19)

`732a837` added the kernel's refusal of a version-1 manifest that carries a
dynamic image, but only the version-2 path ever ran: the refusal was pinned
by code and guard O10, never exercised. This slice adds a negative boot
probe that hands the kernel exactly the manifest the refusal exists for, and
asserts the named line appears.

## What changed

- `user/sidecar/src/m2_dyn.rs` — `v1_interp_probe(k, vfs, image_path, name,
  budget…)`: reads the **same** rootfs dynamic hello
  (`/tmp/m2.dyn.hello`), packs a `version_major: 1` manifest with no
  `INTERP` record (v1 cannot carry one), and calls `linux_spawn`. It returns
  the kernel's refusal code; `Err` means the kernel admitted a dynamic image
  with no loader, i.e. the probe's failure.
- `user/sidecar/src/entry.rs` — the probe runs once, in the same gen-0 block
  as the M2a/M2d one-shots, immediately after the accepted M2d spawn. It
  logs `[M2D] v1 CAP_EINVAL rc=…` only for CAP_EINVAL (-1), so an unrelated
  refusal cannot pass as this one.
- `tests/linux_personality_check.sh` — O10 additionally pins
  `v1_interp_probe`, its `version_major: 1` manifest and its call site, so
  removing the probe reddens the clause by name.
- `tests/m2_dyn_v1_boot_check.sh` (new, `GUARD-KIND: runtime`) — boots the
  M2d ISO and asserts the kernel's own line, the sidecar's CAP_EINVAL, no
  admission, and that the version-2 child still runs.

## Build / boot

```sh
make x86-iso-e3 M2A_INTERP=1 E3_INIT_FEATURES=e3_envs,e7_gate \
     X86_E3_ISO=/tmp/m2dyn_v1.iso SIDECAR_E3_CPIO=/tmp/m2dyn_v1.cpio
timeout 70 qemu-system-x86_64 -cdrom /tmp/m2dyn_v1.iso -m 4G -smp 4 \
     -accel tcg -display none -monitor none -no-reboot -boot d \
     -serial file:/tmp/m2dyn_v1_boot.log
```

ISO SHA-256: `ce4c638f…` (19820544 bytes).

## Live trace

```text
[M2D] staged image=00003468
[M2D] staged loader=000B29E0
[M2D] hello: spawned ch_00000015            <- the accepted v2 child
[CS] CAP_EINVAL: PT_INTERP requires a version-2 manifest (version=1, path='/lib/ld-musl-x86_64.so.1')
[TORE] address space 0x00000000136a8000 destroyed: 1 frame(s) freed
[M2D] v1 CAP_EINVAL rc=FFFFFFFF             <- -1, the sidecar's view
```

The refusal lands after the create cloned its page table and before any
process slot is published, so the teardown frees exactly that one frame; no
`m2.dyn.v1probe.0` process, no `[M2D] v1 ADMITTED`, no
`[M2D] hello: spawn FAILED`. The same boot still prints `M2DYN-OK` (v2
sibling, write len=9) and `M2A-OK` (fixture route), so the negative probe
neither replaced nor disturbed the accepted paths.

## Checks

- `cargo test --features m2a_interp` — exit 0.
- `tests/linux_personality_check.sh` under WSL — all clauses hold, including
  the extended O10.
- ISO build — exit 0, sha above.
- `tests/m2_dyn_v1_boot_check.sh` — 5/5 assertions pass on the built ISO
  (kernel line, sidecar CAP_EINVAL, no admission, v2 spawn, M2DYN-OK).
- Not run: `linux_personality_check_smoke.sh` (15 min); O10's existing tooth
  t155 still reddens the clause, and the boot check is the new live tooth.
